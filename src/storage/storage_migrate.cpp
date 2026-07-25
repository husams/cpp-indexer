// Schema migrations: every version step plus the two one-off table rebuilds.
// Split out of storage.cpp; Storage's interface is unchanged.
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "catalogs/generated_catalog.hpp"
#include "compiledb/compiledb.hpp"
#include "storage/storage_detail.hpp"
#include "storage/storage_schema.hpp"
#include "util/errors.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {

using namespace detail;

void SqliteStorageService::migrate() {
  std::vector<std::string> tables;
  {
    auto st =
        db_.prepare("SELECT name FROM sqlite_master WHERE type = 'table'");
    while (st.step()) {
      tables.push_back(st.col_text(0));
    }
  }
  const auto has_table = [&tables](const char *name) {
    return std::ranges::find(tables, name) != tables.end();
  };
  if (!has_table("symbol")) {
    return; // fresh database: the schema script creates everything
  }
  if (has_table("storage_enum_catalog")) {
    db_.exec("DROP VIEW IF EXISTS edge_site_read");
    db_.exec("DROP VIEW IF EXISTS call_arg_read");
    db_.exec("DROP TABLE storage_enum_catalog");
  }
  const auto table_columns = [this](const char *table) {
    std::vector<std::string> cols;
    auto st = db_.prepare(std::string("PRAGMA table_info(") + table + ")");
    while (st.step()) {
      cols.push_back(st.col_text(1));
    }
    return cols;
  };
  const auto has_col = [](const std::vector<std::string> &cols,
                          const char *name) {
    return std::ranges::find(cols, name) != cols.end();
  };

  const auto cols = table_columns("symbol");
  bool changed = false;
  // v37 -> v38: add the manifest-governed artifact tables. The schema script
  // creates the tables after this probe; only the version row is changed here
  // so old databases remain readable during the same open. This also keeps
  // older artifact-less databases on the ordered migration path.
  if (!has_table("artifact")) {
    changed = true;
  }
  // v35 -> v36: align the sidecar envelope with the generated semantic
  // artifact contract. SQLite cannot alter the catalog type or CHECK values,
  // so rebuild the small manifest family while preserving every child row.
  if (has_table("artifact")) {
    const auto artifact_cols = table_columns("artifact");
    if (!has_col(artifact_cols, "catalog_hash")) {
      db_.exec("PRAGMA foreign_keys = OFF");
      for (const char *table : {"artifact_relation", "artifact_identity_map",
                                "artifact_lease", "artifact_pin"}) {
        if (has_table(table)) {
          db_.exec(std::string("ALTER TABLE ") + table + " RENAME TO " + table +
                   "_v35");
        }
      }
      db_.exec("DROP INDEX IF EXISTS idx_artifact_current_logical");
      db_.exec("DROP INDEX IF EXISTS idx_artifact_state");
      db_.exec("DROP INDEX IF EXISTS idx_artifact_identity_stable");
      db_.exec("ALTER TABLE artifact RENAME TO artifact_v35");
      db_.exec(
          "CREATE TABLE artifact (id INTEGER PRIMARY KEY, logical_id TEXT NOT "
          "NULL, kind TEXT NOT NULL, artifact_schema TEXT NOT NULL, "
          "catalog_version INTEGER NOT NULL, catalog_hash TEXT NOT NULL, "
          "producer_version TEXT NOT NULL, engine_version TEXT NOT NULL, "
          "workspace_identity TEXT NOT NULL, tu_identity TEXT NOT NULL DEFAULT "
          "'', "
          "configuration_identity TEXT NOT NULL DEFAULT '', "
          "input_fact_set_identity TEXT NOT NULL DEFAULT '', completeness TEXT "
          "NOT NULL CHECK (completeness IN ('complete','partial','unknown')), "
          "truncation TEXT NOT NULL CHECK (truncation IN "
          "('none','truncated','unknown')), "
          "trust TEXT NOT NULL CHECK (trust IN "
          "('unverified','producer-verified','reader-verified')), "
          "evidence TEXT NOT NULL CHECK (evidence IN "
          "('source','derived','inferred','runtime','assumption','proof')), "
          "attachment_name TEXT NOT NULL, retention_policy TEXT NOT NULL "
          "DEFAULT 'retain', "
          "relative_path TEXT NOT NULL, content_hash TEXT NOT NULL, byte_size "
          "INTEGER NOT NULL CHECK (byte_size >= 0), "
          "state TEXT NOT NULL CHECK (state IN ('current','stale','retired')), "
          "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, published_at "
          "TEXT, "
          "UNIQUE (logical_id, content_hash))");
      db_.exec(
          "INSERT INTO artifact(id, logical_id, kind, artifact_schema, "
          "catalog_version, catalog_hash, "
          "producer_version, engine_version, workspace_identity, "
          "tu_identity, configuration_identity, "
          "input_fact_set_identity, completeness, truncation, trust, "
          "evidence, attachment_name, "
          "retention_policy, relative_path, content_hash, byte_size, "
          "state, created_at, published_at) "
          "SELECT id, logical_id, kind, artifact_schema, "
          "CASE WHEN catalog_version GLOB '[0-9]*' AND "
          "catalog_version NOT GLOB '*[^0-9]*' THEN "
          "CAST(catalog_version AS INTEGER) ELSE 0 END, '', "
          "producer_version, engine_version, workspace_identity, "
          "tu_identity, configuration_identity, input_fact_set_identity, "
          "completeness, truncation, 'unverified', 'assumption', "
          "attachment_name, retention_policy, relative_path, content_hash, "
          "byte_size, CASE WHEN state = 'current' THEN 'stale' ELSE state "
          "END, created_at, published_at "
          "FROM artifact_v35");
      db_.exec("CREATE UNIQUE INDEX idx_artifact_current_logical ON "
               "artifact(logical_id) WHERE state = 'current';"
               "CREATE INDEX idx_artifact_state ON artifact(state);");
      db_.exec(
          "CREATE TABLE artifact_relation (artifact_id INTEGER NOT NULL "
          "REFERENCES artifact(id) ON DELETE CASCADE, relation_name TEXT NOT "
          "NULL, PRIMARY KEY (artifact_id, relation_name)) WITHOUT ROWID;");
      db_.exec(
          "INSERT INTO artifact_relation SELECT * FROM artifact_relation_v35;");
      db_.exec(
          "CREATE TABLE artifact_identity_map (artifact_id INTEGER NOT NULL "
          "REFERENCES artifact(id) ON DELETE CASCADE, local_identity TEXT NOT "
          "NULL, identity_kind TEXT NOT NULL, stable_identity TEXT NOT NULL, "
          "resolution_state TEXT NOT NULL CHECK (resolution_state IN "
          "('resolved','unresolved','unknown')), core_symbol_id INTEGER, "
          "diagnostic TEXT NOT NULL DEFAULT '', PRIMARY KEY (artifact_id, "
          "local_identity, identity_kind)) WITHOUT ROWID;");
      db_.exec("INSERT INTO artifact_identity_map SELECT * FROM "
               "artifact_identity_map_v35;");
      db_.exec("CREATE INDEX idx_artifact_identity_stable ON "
               "artifact_identity_map(stable_identity);");
      db_.exec("CREATE TABLE artifact_lease (artifact_id INTEGER NOT NULL "
               "REFERENCES artifact(id) ON DELETE CASCADE, lease_id TEXT NOT "
               "NULL, purpose TEXT NOT NULL, PRIMARY KEY (artifact_id, "
               "lease_id)) WITHOUT ROWID;");
      db_.exec("INSERT INTO artifact_lease SELECT * FROM artifact_lease_v35;");
      db_.exec(
          "CREATE TABLE artifact_pin (artifact_id INTEGER NOT NULL REFERENCES "
          "artifact(id) ON DELETE CASCADE, pin_id TEXT NOT NULL, reason TEXT "
          "NOT NULL, PRIMARY KEY (artifact_id, pin_id)) WITHOUT ROWID;");
      db_.exec("INSERT INTO artifact_pin SELECT * FROM artifact_pin_v35;");
      for (const char *table : {"artifact_relation", "artifact_identity_map",
                                "artifact_lease", "artifact_pin"}) {
        if (has_table(table)) {
          db_.exec(std::string("DROP TABLE ") + table + "_v35");
        }
      }
      db_.exec("DROP TABLE artifact_v35");
      db_.exec("PRAGMA foreign_keys = ON");
      changed = true;
    }
  }
  if (has_table("artifact")) {
    auto legacy_hashes = db_.prepare(
        "UPDATE artifact SET content_hash = 'legacy-sha1:' || content_hash, "
        "state = CASE WHEN state = 'current' THEN 'stale' ELSE state END "
        "WHERE content_hash NOT LIKE 'sha256:%' AND content_hash NOT LIKE "
        "'legacy-sha1:%'");
    legacy_hashes.step_done();
    if (db_.changes() > 0) {
      changed = true;
    }
  }
  if (!has_col(cols, "qual_name")) {
    db_.exec("ALTER TABLE symbol ADD COLUMN qual_name TEXT");
    db_.exec(kQualNameBackfill);
    changed = true;
  }
  if (!has_col(cols, "decl_file_id")) {
    db_.exec("ALTER TABLE symbol ADD COLUMN decl_file_id INTEGER "
             "REFERENCES file(id) ON DELETE SET NULL");
    db_.exec("ALTER TABLE symbol ADD COLUMN decl_line INTEGER");
    db_.exec("ALTER TABLE symbol ADD COLUMN decl_col INTEGER");
    db_.exec("UPDATE symbol SET decl_file_id = file_id, decl_line = line, "
             "decl_col = col WHERE is_definition = 0");
    changed = true;
  }
  if (!has_col(cols, "is_pure")) {
    // No backfill possible from stored data -- reindex to populate.
    db_.exec(
        "ALTER TABLE symbol ADD COLUMN is_pure INTEGER NOT NULL DEFAULT 0");
    changed = true;
  }
  if (!has_col(cols, "is_static")) {
    // v11 -> v12: C++ static member function flag. No backfill possible from
    // stored data -- reindex to populate; old rows read as 0.
    db_.exec(
        "ALTER TABLE symbol ADD COLUMN is_static INTEGER NOT NULL DEFAULT 0");
    changed = true;
  }
  if (!has_col(cols, "is_instantiation")) {
    // v12 -> v13: implicit template-instantiation node marker. No backfill
    // possible from stored data -- reindex to populate; old rows read as 0.
    db_.exec("ALTER TABLE symbol ADD COLUMN is_instantiation INTEGER NOT NULL "
             "DEFAULT 0");
    changed = true;
  }
  if (!has_col(cols, "decl_path")) {
    // v8 -> v9: raw decl path for stubs whose target lives in an unregistered
    // (system/stdlib) file. No backfill -- those rows had no location to
    // recover; a reindex repopulates it from the AST.
    db_.exec("ALTER TABLE symbol ADD COLUMN decl_path TEXT");
    changed = true;
  }
  // v15 -> v16: symbol.kind moves from a TEXT name to its CXCursorKind integer
  // (compact storage; the symbol_kind table recovers the string). The column
  // type changes and the old CHECK must go, so the table is rebuilt in place
  // with the kind values converted. Runs after the column-add migrations above
  // so the new table mirrors all columns. Mirrors storage.py.
  {
    std::string kind_type;
    {
      // Read the kind column's declared type. Finalize this statement (close
      // the scope) BEFORE the rebuild -- an open read on `symbol` would make
      // DROP TABLE fail with "database table is locked".
      auto st = db_.prepare("PRAGMA table_info(symbol)");
      while (st.step()) {
        if (st.col_text(1) == "kind") {
          kind_type = st.col_text(2);
        }
      }
    }
    for (char &c : kind_type) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (kind_type != "INTEGER") {
      migrate_symbol_kind_to_int();
      changed = true;
    }
  }
  // v19 -> v20: named-instance marker. A template instance minted from a NAMED
  // using/typedef alias (X<B>) carries its own composes/aggregates/associates
  // instead of collapsing onto the primary. No backfill -- reindex repopulates;
  // old rows read as 0. Re-read columns: the v15->v16 rebuild above recreates
  // `symbol` (without this column), so the snapshot from the top is stale.
  {
    const auto cols2 = table_columns("symbol");
    if (!has_col(cols2, "is_named_instance")) {
      db_.exec(
          "ALTER TABLE symbol ADD COLUMN is_named_instance INTEGER NOT NULL "
          "DEFAULT 0");
      changed = true;
    }
    // v24 -> v25: end of the symbol's own extent (end_line/end_col), paired
    // with (line, col). Only the START was stored before, so nothing to
    // backfill -- old rows read NULL until a reindex populates them from
    // cursor.extent.end.
    if (!has_col(cols2, "end_line")) {
      db_.exec("ALTER TABLE symbol ADD COLUMN end_line INTEGER");
      db_.exec("ALTER TABLE symbol ADD COLUMN end_col INTEGER");
      changed = true;
    }
    // v26 -> v27: count of definitions of this symbol (>1 == redefined per
    // backend). No backfill -- a reindex + resolve populates it (definition
    // rows are written at index, counted at resolve); old rows read 0.
    if (!has_col(cols2, "multi_def")) {
      db_.exec("ALTER TABLE symbol ADD COLUMN multi_def INTEGER NOT NULL "
               "DEFAULT 0");
      changed = true;
    }
  }
  // v27 -> v28: per-backend initializer text on a (static member) variable
  // definition. No backfill -- a reindex repopulates it; old rows read NULL.
  if (has_table("definition")) {
    const auto dcols = table_columns("definition");
    if (!has_col(dcols, "init_text")) {
      db_.exec("ALTER TABLE definition ADD COLUMN init_text TEXT");
      changed = true;
    }
  }
  if (has_table("file")) {
    const auto fcols = table_columns("file");
    if (!has_col(fcols, "driver")) {
      // No backfill possible from stored data -- re-import to populate.
      db_.exec("ALTER TABLE file ADD COLUMN driver TEXT");
      changed = true;
    }
    // v7 -> v8: per-file flag override marker (`cidx file`). Existing rows
    // default to 0 (not overridden), so re-import behaves as before.
    if (!has_col(fcols, "args_overridden")) {
      db_.exec("ALTER TABLE file ADD COLUMN args_overridden INTEGER "
               "NOT NULL DEFAULT 0");
      changed = true;
    }
  }
  // v9 -> v10: receiver provenance + per-argument provenance for virtual
  // dispatch. No backfill -- reindex repopulates from the AST.
  if (has_table("edge_site")) {
    const auto escols = table_columns("edge_site");
    if (!has_col(escols, "recv_src_kind")) {
      db_.exec("ALTER TABLE edge_site ADD COLUMN recv_src_kind TEXT");
      db_.exec("ALTER TABLE edge_site ADD COLUMN recv_type_usr TEXT");
      db_.exec("ALTER TABLE edge_site ADD COLUMN recv_decl_usr TEXT");
      changed = true;
    }
    if (!has_col(escols, "recv_param_pos")) {
      db_.exec("ALTER TABLE edge_site ADD COLUMN recv_param_pos INTEGER");
      changed = true;
    }
    if (!has_table("call_arg")) {
      // The call_arg table itself is created by the schema script (CREATE
      // TABLE IF NOT EXISTS), run after migrate(), so we only flip changed
      // to bump the version -- identical to the v6->v7 graph tables pattern.
      changed = true;
    }
    // v10 -> v11: value-ness booleans for exact-singleton Gamma narrowing.
    // No backfill -- reindex repopulates; old rows read as NULL == not-value ==
    // TOP.
    if (!has_col(escols, "recv_type_is_value")) {
      db_.exec("ALTER TABLE edge_site ADD COLUMN recv_type_is_value INTEGER");
      changed = true;
    }
  }
  if (has_table("call_arg")) {
    const auto cacols = table_columns("call_arg");
    if (!has_col(cacols, "type_is_value")) {
      db_.exec("ALTER TABLE call_arg ADD COLUMN type_is_value INTEGER");
      changed = true;
    }
  }
  // v6 -> v7: the graph tables are created by the schema script (CREATE TABLE
  // IF NOT EXISTS + INSERT OR IGNORE edge_kind). No symbol/file ALTER is
  // needed, so detect the version from meta and bump it directly. Idempotent.
  if (!has_table("edge")) {
    changed = true; // tables will be created by the schema script
  } else {
    // edge table exists: bump version only when stored version is OLDER
    // (future-schema DBs — version > kSchemaVersion — are left untouched so
    // we do NOT downgrade them).
    auto st =
        db_.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    if (st.step()) {
      const std::string v = st.col_text(0);
      if (!v.empty() && std::stoi(v) < kSchemaVersion) {
        changed = true; // forces the meta UPDATE below to write the new version
      }
    }
  }
  // v13 -> v14: component.version column + label table.
  // component.version: guard on PRAGMA table_info(component).
  if (has_table("component")) {
    const auto ccols = table_columns("component");
    if (!has_col(ccols, "version")) {
      // No backfill — existing components get version = NULL.
      db_.exec("ALTER TABLE component ADD COLUMN version TEXT");
      changed = true;
    }
  }
  // label table: created by the schema script (CREATE TABLE IF NOT EXISTS).
  // Only flip changed here so the meta UPDATE fires.
  if (!has_table("label")) {
    changed = true; // table will be created by the schema script
  }
  // v22 -> v23: repository + clone tables and component.repository_id. The two
  // tables are created by the schema script (CREATE TABLE IF NOT EXISTS); only
  // the new component column needs an ALTER. No backfill -- existing components
  // stay ungrouped (repository_id NULL) until a re-import / `cidx repo`
  // command.
  if (has_table("component")) {
    const auto ccols2 = table_columns("component");
    if (!has_col(ccols2, "repository_id")) {
      db_.exec("ALTER TABLE component ADD COLUMN repository_id INTEGER");
      changed = true;
    }
  }
  if (!has_table("repository")) {
    changed = true; // table will be created by the schema script
  }
  // v23 -> v24: `component.path` is now UNIQUE per repository (was global).
  // Rebuild the table when the old global UNIQUE(path) is still in place so the
  // clone-relative paths below (multiple '.' roots) do not collide. Mirrors
  // storage.py.
  if (has_table("component")) {
    std::string comp_sql;
    { // scope the read statement so it is finalized before the table rebuild
      auto sq = db_.prepare("SELECT sql FROM sqlite_master "
                            "WHERE type = 'table' AND name = 'component'");
      if (sq.step()) {
        comp_sql = sq.col_text(0);
      }
    }
    if (!comp_sql.empty() &&
        !comp_sql.contains("UNIQUE (repository_id, path)")) {
      migrate_component_repo_unique();
      changed = true;
    }
  }
  // v23 -> v24: a grouped component's path becomes RELATIVE to its repository's
  // active clone root, so `repo switch` only repoints the active pointer (no
  // per-component rewrite). Convert any component that is grouped, has a live
  // active clone, stores an ABSOLUTE path (not version-in-path), and sits under
  // that clone -- strip the clone prefix (`.` when it IS the clone root).
  // Idempotent: an already-relative path is skipped. Mirrors storage.py.
  if (has_table("repository") && has_table("clone") && has_table("component") &&
      has_col(table_columns("component"), "repository_id")) {
    struct Pending {
      int64_t id;
      std::string rel;
    };
    std::vector<Pending> pend;
    { // scope the read statement so it is finalized before the UPDATEs below
      auto scan = db_.prepare("SELECT id, path, repository_id FROM component "
                              "WHERE repository_id IS NOT NULL");
      while (scan.step()) {
        const int64_t cid = scan.col_int64(0);
        const std::string cpath = scan.col_text(1);
        const int64_t rid = scan.col_int64(2);
        if (cpath.contains('<') || cpath.contains('$') ||
            !pathutil::isabs(cpath)) {
          continue; // portable or already relative
        }
        if (!CompileDb::split_base_version(cpath).second.empty()) {
          continue; // version-in-path: keep absolute (see relativize_component)
        }
        std::optional<int64_t> active;
        {
          auto rst = db_.prepare(
              "SELECT active_clone_id FROM repository WHERE id = ?");
          rst.bind(1, rid);
          if (!rst.step()) {
            continue;
          }
          active = opt_int64(rst, 0);
        }
        if (!active) {
          continue;
        }
        std::string clone_path;
        {
          auto cst = db_.prepare("SELECT path FROM clone WHERE id = ?");
          cst.bind(1, *active);
          if (!cst.step()) {
            continue;
          }
          clone_path = cst.col_text(0);
        }
        std::string root =
            pathutil::abspath(pathutil::resolve_fs_path(clone_path));
        while (!root.empty() && root.back() == '/') {
          root.pop_back();
        }
        std::string base = pathutil::abspath(cpath);
        while (!base.empty() && base.back() == '/') {
          base.pop_back();
        }
        std::string rel;
        if (base == root) {
          rel = ".";
        } else if (base.starts_with(root + "/")) {
          rel = pathutil::relpath(base, root);
        } else {
          continue; // component outside the active clone -> keep absolute
        }
        pend.push_back({.id = cid, .rel = rel});
      }
    } // scan finalized
    for (const Pending &p : pend) {
      auto upd = db_.prepare("UPDATE component SET path = ? WHERE id = ?");
      upd.bind(1, std::string_view(p.rel));
      upd.bind(2, p.id);
      upd.step_done();
      changed = true;
    }
  }
  // v14 -> v15: per-file parse diagnostics. Created by the schema script
  // (CREATE TABLE IF NOT EXISTS); no backfill -- a reindex repopulates it.
  if (!has_table("diagnostic")) {
    changed = true; // table will be created by the schema script
  }
  // v16 -> v17: Layer-1 entity_edge + entity_edge_kind tables. Created by the
  // schema script (CREATE TABLE IF NOT EXISTS). entity_edge is a derived,
  // materialised table -- populate via `cidx resolve`. No backfill on
  // migration.
  if (!has_table("entity_edge")) {
    changed = true; // tables will be created by the schema script
  } else {
    // The `nests` entity_edge kind was removed (lexical nesting is a symbol
    // declaration-scope property, not a relation). Clean the DB in place: drop
    // the defunct nests rows (kind 10) and renumber befriends 11 -> 10 to match
    // the new contiguous seed. Order matters -- delete the old kind-10 rows
    // BEFORE renumbering 11 -> 10 so the two never collide on
    // UNIQUE(src,dst,kind,via). Also drop the stale entity_edge_kind rows so
    // the schema script's INSERT OR IGNORE reseeds (10,'befriends'). Mirrors
    // storage.py.
    //
    // Gate on the STALE DATA (a leftover 'nests' seed row), NOT the schema
    // version: an earlier build bumped schema_version to 18 WITHOUT cleaning,
    // so a version gate would skip those already-stamped DBs. Idempotent --
    // after cleanup there is no 'nests' row, so it never runs again.
    bool stale = false;
    {
      auto st = db_.prepare(
          "SELECT 1 FROM entity_edge_kind WHERE name = 'nests' LIMIT 1");
      stale = st.step();
    }
    if (stale) {
      db_.exec("DELETE FROM entity_edge WHERE kind = 10");
      db_.exec("UPDATE entity_edge SET kind = 10 WHERE kind = 11");
      db_.exec("DELETE FROM entity_edge_kind WHERE id IN (10, 11)");
      changed = true;
    }
    // Rename kind 2 'realizes' -> 'implements' (display name only; the stored
    // entity_edge.kind int is unchanged). Data-gated on the old name so it
    // fires regardless of schema_version; the schema script's INSERT OR IGNORE
    // would otherwise leave the stale (2,'realizes') row in place. Mirrors
    // storage.py.
    bool renamed = false;
    {
      auto st = db_.prepare(
          "SELECT 1 FROM entity_edge_kind WHERE id = 2 AND name = 'realizes'");
      renamed = st.step();
    }
    if (renamed) {
      db_.exec("UPDATE entity_edge_kind SET name = 'implements' WHERE id = 2");
      changed = true;
    }
    // v20 -> v21: NULL-safe entity_edge identity. The old table-level
    // UNIQUE(src,dst,kind,via_member_id) never collided on NULL-via rows
    // (SQLite NULL != NULL), so every materialise fanned NULL-via edges out
    // into duplicate copies. The schema script now builds a COALESCE unique
    // index idx_entity_edge_identity; it would fail to create over a DB that
    // already carries those duplicates, so dedup in place first (keep the
    // lowest rowid per logical key). Gate on the index's absence so it runs
    // exactly once; entity_edge is derived, so `cidx resolve` repopulates
    // cleanly. Mirrors storage.py.
    bool has_identity_idx = false;
    {
      auto st = db_.prepare("SELECT 1 FROM sqlite_master WHERE type = 'index' "
                            "AND name = 'idx_entity_edge_identity'");
      has_identity_idx = st.step();
    }
    if (!has_identity_idx) {
      db_.exec("DELETE FROM entity_edge WHERE rowid NOT IN ("
               "  SELECT MIN(rowid) FROM entity_edge GROUP BY "
               "    src_id, dst_id, kind, "
               "    COALESCE(via_member_id, -1), COALESCE(create_form, -1))");
      changed = true;
    }
  }
  // v21 -> v22: entity_node + entity_kind tables (the entity's design type).
  // The table is created by the schema script (run after migrate); the
  // constructor backfills it from existing symbols right after -- pure-DB, no
  // re-index/resolve. Mirrors storage.py.
  if (!has_table("entity_node")) {
    needs_entity_node_backfill_ = true;
    changed = true;
  }
  // v25 -> v26: per-symbol declaration/reopen sites (decl_site). Created by the
  // schema script (CREATE TABLE IF NOT EXISTS). No backfill from stored rows is
  // possible -- only the winning site survives on the symbol row -- so a
  // reindex repopulates every site; bump the version so the DB is stamped v26.
  // Mirrors storage.py.
  if (!has_table("decl_site")) {
    changed = true;
  }
  // v28 -> v29: canonical template_arg.arg_kind
  // (docs/improvements/template-arg-contract.md). The class-spec extraction
  // path used to store raw CXTemplateArgumentKind values; the contract codes
  // are 1=type 2=non-type 3=template-template 4=pack. VERSION-gated, not
  // data-gated: after the remap a legitimate template-template row (3) on a
  // record-like owner is indistinguishable from a legacy NullPtr row, so this
  // must run exactly once. Legacy 3 (NullPtr) only ever occurred on
  // record-like owners — the callable paths always wrote in-contract codes —
  // and the ambiguous 3 must remap BEFORE 5/6 -> 3 mints new, valid 3s.
  // Mirrors storage.py.
  if (has_table("template_arg")) {
    int stored = 0;
    {
      auto st =
          db_.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
      if (st.step()) {
        const std::string v = st.col_text(0);
        if (!v.empty()) {
          stored = std::stoi(v);
        }
      }
    }
    if (stored > 0 && stored < 29) {
      db_.exec("UPDATE template_arg SET arg_kind = 2 WHERE arg_kind = 3 "
               "AND owner_id IN (SELECT id FROM symbol "
               "                 WHERE kind IN (2, 3, 4, 31))");
      db_.exec("UPDATE template_arg SET arg_kind = 3 WHERE arg_kind IN (5, 6)");
      db_.exec("UPDATE template_arg SET arg_kind = 2 WHERE arg_kind = 7");
      db_.exec("UPDATE template_arg SET arg_kind = 4 WHERE arg_kind = 8");
      db_.exec("DELETE FROM template_arg WHERE arg_kind = 0");
      changed = true;
    }
  }
  // v29 -> v30: signature/type tier (type_node/type_edge/parameter/symbol_type
  // + their seed tables). All created by the schema script (CREATE TABLE IF
  // NOT EXISTS); no backfill is possible from stored rows -- a reindex
  // populates them. Bump the version so the DB is stamped v30. Mirrors
  // storage.py.
  if (!has_table("type_node")) {
    changed = true; // tables will be created by the schema script
  }
  // v30 -> v31: include tier (include_config/include_edge/include_site/
  // include_macro_use + the directive seed table). All created by the schema
  // script (CREATE TABLE IF NOT EXISTS). Preprocessing facts cannot be
  // recovered from stored rows -- only a reindex populates them, so an
  // upgraded DB has an EMPTY include graph until `cidx index` reruns, and
  // `cidx include` reports that rather than treating empty as "no includes".
  // Mirrors storage.py.
  if (!has_table("include_edge")) {
    changed = true; // tables will be created by the schema script
  }
  // v31 -> v32: complete callable signature metadata. Existing rows retain
  // their adjusted type in type_id; the declaration pass repopulates the
  // source-level and default fields on the next index.
  if (has_table("parameter")) {
    const auto pcols = table_columns("parameter");
    const std::array<std::string_view, 6> added = {
        "pack_index",   "declared_type_id", "adjusted_type_id",
        "default_text", "default_origin",   "reference_semantics"};
    for (const auto col : added) {
      if (!has_col(pcols, std::string(col).c_str())) {
        db_.exec("ALTER TABLE parameter ADD COLUMN " + std::string(col) +
                 (col == "pack_index"    ? " INTEGER NOT NULL DEFAULT -1"
                  : col.ends_with("_id") ? " INTEGER"
                                         : " TEXT"));
        changed = true;
      }
    }
  }
  if (has_table("parameter")) {
    std::string param_sql;
    {
      auto st = db_.prepare("SELECT sql FROM sqlite_master WHERE type='table' "
                            "AND name='parameter'");
      if (st.step())
        param_sql = st.col_text(0);
    }
    if (!param_sql.contains("PRIMARY KEY (owner_id, position, pack_index)")) {
      db_.exec("PRAGMA foreign_keys = OFF");
      db_.exec(
          "CREATE TABLE parameter_v32 (owner_id INTEGER NOT NULL REFERENCES "
          "symbol(id) ON DELETE CASCADE, position INTEGER NOT NULL, pack_index "
          "INTEGER NOT NULL DEFAULT -1, name TEXT, type_id INTEGER REFERENCES "
          "type_node(id) ON DELETE SET NULL, declared_type_id INTEGER "
          "REFERENCES "
          "type_node(id) ON DELETE SET NULL, adjusted_type_id INTEGER "
          "REFERENCES "
          "type_node(id) ON DELETE SET NULL, default_text TEXT, default_origin "
          "TEXT, reference_semantics TEXT, file_id INTEGER REFERENCES file(id) "
          "ON DELETE SET NULL, line INTEGER, col INTEGER, PRIMARY KEY "
          "(owner_id, "
          "position, pack_index)) WITHOUT ROWID");
      db_.exec("INSERT INTO parameter_v32 SELECT owner_id, position, "
               "pack_index, name, type_id, declared_type_id, adjusted_type_id, "
               "default_text, default_origin, reference_semantics, file_id, "
               "line, col FROM parameter");
      db_.exec("DROP TABLE parameter");
      db_.exec("ALTER TABLE parameter_v32 RENAME TO parameter");
      db_.exec("PRAGMA foreign_keys = ON");
      changed = true;
    }
  }
  if (has_table("template_param")) {
    const auto tcols = table_columns("template_param");
    const std::array<std::string_view, 3> added = {"type_id", "default_type_id",
                                                   "default_ref_id"};
    for (const auto col : added) {
      if (!has_col(tcols, std::string(col).c_str())) {
        db_.exec("ALTER TABLE template_param ADD COLUMN " + std::string(col) +
                 " INTEGER");
        changed = true;
      }
    }
  }
  if (has_table("template_arg")) {
    const auto acols = table_columns("template_arg");
    if (!has_col(acols, "pack_index")) {
      db_.exec("ALTER TABLE template_arg ADD COLUMN pack_index INTEGER "
               "NOT NULL DEFAULT -1");
      changed = true;
    }
    if (!has_col(acols, "type_id")) {
      db_.exec("ALTER TABLE template_arg ADD COLUMN type_id INTEGER");
      changed = true;
    }
    std::string arg_sql;
    {
      auto st = db_.prepare("SELECT sql FROM sqlite_master WHERE type='table' "
                            "AND name='template_arg'");
      if (st.step())
        arg_sql = st.col_text(0);
    }
    if (!arg_sql.contains("PRIMARY KEY (owner_id, position, pack_index)")) {
      db_.exec("PRAGMA foreign_keys = OFF");
      db_.exec(
          "CREATE TABLE template_arg_v32 (owner_id INTEGER NOT NULL REFERENCES "
          "symbol(id) ON DELETE CASCADE, position INTEGER NOT NULL, pack_index "
          "INTEGER NOT NULL DEFAULT -1, arg_kind INTEGER NOT NULL, ref_id "
          "INTEGER REFERENCES symbol(id) ON DELETE SET NULL, literal TEXT, "
          "type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL, "
          "PRIMARY KEY (owner_id, position, pack_index)) WITHOUT ROWID");
      db_.exec(
          "INSERT INTO template_arg_v32 SELECT owner_id, position, pack_index, "
          "arg_kind, ref_id, literal, type_id FROM template_arg");
      db_.exec("DROP TABLE template_arg");
      db_.exec("ALTER TABLE template_arg_v32 RENAME TO template_arg");
      db_.exec("PRAGMA foreign_keys = ON");
      changed = true;
    }
  }
  // v32 -> v33: the evaluated constant value of a variable initializer /
  // enumerator on symbol. No backfill is possible from stored rows -- a
  // reindex populates it; old rows read NULL until then. Mirrors storage.py.
  {
    const auto scols = table_columns("symbol");
    if (!has_col(scols, "const_value")) {
      db_.exec("ALTER TABLE symbol ADD COLUMN const_value TEXT");
      changed = true;
    }
  }
  // v33 -> v34: dedicated alias_of(19) edge kind. The typedef/using-alias ->
  // underlying-type edge was previously stored as the overloaded uses(7);
  // rewrite exactly those rows: source is an alias symbol (typedef 20 /
  // type-alias 36) and the target is not a namespace (a qualified alias like
  // `using X = ns::Foo` also carries an alias -> ns namespace-qualifier
  // uses(7) edge, which stays a use). Mirrors storage.py.
  if (has_table("edge_kind")) {
    auto probe = db_.prepare("SELECT 1 FROM edge_kind WHERE id = 19");
    if (!probe.step()) {
      db_.exec("INSERT OR IGNORE INTO edge_kind (id, name) "
               "VALUES (19,'alias_of')");
      db_.exec("UPDATE edge SET kind = 19 WHERE kind = 7 AND src_id IN "
               "(SELECT id FROM symbol WHERE kind IN (20, 36)) "
               "AND dst_id NOT IN (SELECT id FROM symbol WHERE kind = 22)");
      changed = true;
    }
    // Same step, second rewrite: a variable(9) / member(6) -> its declared
    // type becomes of_type(20). Namespace-qualifier edges are excluded
    // exactly as above.
    auto probe20 = db_.prepare("SELECT 1 FROM edge_kind WHERE id = 20");
    if (!probe20.step()) {
      db_.exec("INSERT OR IGNORE INTO edge_kind (id, name) "
               "VALUES (20,'of_type')");
      db_.exec("UPDATE edge SET kind = 20 WHERE kind = 7 AND src_id IN "
               "(SELECT id FROM symbol WHERE kind IN (6, 9)) "
               "AND dst_id NOT IN (SELECT id FROM symbol WHERE kind = 22)");
      changed = true;
    }
  }
  // v34 -> v35: add the shared normalized configuration identity while
  // retaining the v31 include_config columns for current API compatibility.
  if (has_table("include_config")) {
    const auto include_cols = table_columns("include_config");
    if (!has_col(include_cols, "translation_unit_config_id")) {
      db_.exec("ALTER TABLE include_config ADD COLUMN "
               "translation_unit_config_id INTEGER REFERENCES "
               "translation_unit_config(id) ON DELETE SET NULL");
      changed = true;
    }
  }
  // v38 -> v39: make the symbol USR an explicitly scoped identity. Existing
  // unscoped rows belong to the legacy single-workspace universe; preserve
  // their ids and all graph foreign keys while rebuilding the old global-USR
  // table.
  {
    const bool universe_missing = !has_table("semantic_universe");
    const bool symbol_scope_missing =
        !has_col(table_columns("symbol"), "semantic_universe_id") ||
        !has_col(table_columns("symbol"), "identity_key");
    const bool repository_scope_missing =
        has_table("repository") &&
        !has_col(table_columns("repository"), "semantic_universe_id");
    const bool component_scope_missing =
        has_table("component") &&
        !has_col(table_columns("component"), "semantic_universe_id");
    if (universe_missing || symbol_scope_missing || repository_scope_missing ||
        component_scope_missing) {
      migrate_symbol_identity_scope();
      changed = true;
    }
  }
  if (!changed) {
    auto st =
        db_.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    if (st.step() && !st.col_text(0).empty() &&
        std::stoi(st.col_text(0)) < kSchemaVersion) {
      changed = true;
    }
  }

  int stored_schema_version = 0;
  {
    auto st =
        db_.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    if (st.step()) {
      const std::string value = st.col_text(0);
      if (!value.empty()) {
        stored_schema_version = std::stoi(value);
      }
    }
  }
  if (stored_schema_version < kSchemaVersion) {
    // v34 -> v35: compact high-cardinality occurrence identities. Resolvable
    // USRs move to local integer FKs; values with no local row are retained
    // once in external_identity and remain distinguishable from NULL evidence.
    // The legacy text columns are intentionally nulled after the lossless
    // backfill; read-side compatibility views reconstruct the public USR
    // fields.
    db_.exec("CREATE TABLE IF NOT EXISTS external_identity ("
             "id INTEGER PRIMARY KEY, identity_kind INTEGER NOT NULL "
             "CHECK (identity_kind IN (1,2,3)), "
             "identity_text TEXT NOT NULL, resolution_status INTEGER NOT NULL "
             "DEFAULT 0 CHECK (resolution_status IN (0,1)), "
             "symbol_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL, "
             "type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL, "
             "UNIQUE(identity_kind, identity_text))");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_external_identity_symbol "
             "ON external_identity(symbol_id)");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_external_identity_type "
             "ON external_identity(type_id)");

    const auto add_col = [this](const char *table, const char *column,
                                const char *definition) {
      auto st = db_.prepare(std::string("PRAGMA table_info(") + table + ")");
      bool present = false;
      while (st.step()) {
        present = present || st.col_text(1) == column;
      }
      if (!present) {
        db_.exec(std::string("ALTER TABLE ") + table + " ADD COLUMN " + column +
                 " " + definition);
      }
    };
    const bool has_type_node = has_table("type_node");
    const bool has_edge_identity_text =
        has_table("edge_site") &&
        has_col(table_columns("edge_site"), "recv_type_usr") &&
        has_col(table_columns("edge_site"), "recv_decl_usr");
    const bool has_call_identity_text =
        has_table("call_arg") &&
        has_col(table_columns("call_arg"), "type_usr") &&
        has_col(table_columns("call_arg"), "decl_usr") &&
        has_col(table_columns("call_arg"), "callee_usr");
    if (has_table("symbol")) {
      add_col("symbol", "parent_id",
              "INTEGER REFERENCES symbol(id) ON DELETE SET NULL");
      db_.exec("CREATE INDEX IF NOT EXISTS idx_symbol_parent_id ON "
               "symbol(parent_id)");
      db_.exec("UPDATE symbol SET parent_id = (SELECT id FROM symbol p "
               "WHERE p.usr = symbol.parent_usr) WHERE parent_usr IS NOT NULL");
    }
    if (has_table("type_node")) {
      add_col("type_node", "decl_id",
              "INTEGER REFERENCES symbol(id) ON DELETE SET NULL");
      db_.exec("CREATE INDEX IF NOT EXISTS idx_type_node_decl_id ON "
               "type_node(decl_id)");
      db_.exec("UPDATE type_node SET decl_id = (SELECT id FROM symbol s "
               "WHERE s.usr = type_node.decl_usr) WHERE decl_usr IS NOT NULL");
    }
    if (has_table("edge_site")) {
      add_col("edge_site", "recv_src_kind_id", "INTEGER");
      add_col("edge_site", "recv_type_id",
              "INTEGER REFERENCES type_node(id) ON DELETE SET NULL");
      add_col("edge_site", "recv_decl_id",
              "INTEGER REFERENCES symbol(id) ON DELETE SET NULL");
      add_col("edge_site", "recv_type_identity_id",
              "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL");
      add_col("edge_site", "recv_decl_identity_id",
              "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL");
      if (has_col(table_columns("edge_site"), "recv_src_kind")) {
        db_.exec("UPDATE edge_site SET recv_src_kind = NULL WHERE "
                 "recv_src_kind = 'value'");
        for (const auto &entry : catalog::kSourceKinds) {
          db_.exec("UPDATE edge_site SET recv_src_kind_id = " +
                   std::to_string(entry.id) + " WHERE recv_src_kind = '" +
                   std::string(entry.name) + "'");
        }
        auto unknown = db_.prepare("SELECT recv_src_kind FROM edge_site WHERE "
                                   "recv_src_kind IS NOT NULL "
                                   "AND recv_src_kind_id IS NULL LIMIT 1");
        if (unknown.step()) {
          throw StorageError("unknown source kind '" + unknown.col_text(0) +
                             "' in edge_site migration");
        }
        db_.exec("UPDATE edge_site SET recv_src_kind = NULL");
      }
      if (has_edge_identity_text && has_type_node) {
        db_.exec("INSERT OR IGNORE INTO "
                 "external_identity(identity_kind,identity_text,"
                 "resolution_status,type_id) SELECT 1, es.recv_type_usr, "
                 "CASE WHEN tn.id IS NULL THEN 0 ELSE 1 END, tn.id FROM "
                 "edge_site es "
                 "LEFT JOIN type_node tn ON tn.decl_usr = es.recv_type_usr "
                 "WHERE es.recv_type_usr IS NOT NULL");
      }
      if (has_edge_identity_text && has_type_node) {
        db_.exec(
            "INSERT OR IGNORE INTO "
            "external_identity(identity_kind,identity_text,"
            "resolution_status,symbol_id) SELECT 2, es.recv_decl_usr, "
            "CASE WHEN s.id IS NULL THEN 0 ELSE 1 END, s.id FROM edge_site es "
            "LEFT JOIN symbol s ON s.usr = es.recv_decl_usr "
            "WHERE es.recv_decl_usr IS NOT NULL");
        db_.exec("UPDATE edge_site SET recv_decl_id = (SELECT id FROM symbol s "
                 "WHERE s.usr = edge_site.recv_decl_usr), "
                 "recv_decl_identity_id = (SELECT id FROM external_identity i "
                 "WHERE i.identity_kind = 2 AND i.identity_text = "
                 "edge_site.recv_decl_usr)");
      }
      if (has_edge_identity_text && has_type_node) {
        db_.exec(
            "UPDATE edge_site SET recv_type_id = (SELECT id FROM type_node t "
            "WHERE t.decl_usr = edge_site.recv_type_usr), "
            "recv_type_identity_id = (SELECT id FROM external_identity i "
            "WHERE i.identity_kind = 1 AND i.identity_text = "
            "edge_site.recv_type_usr)");
      }
      if (has_edge_identity_text && has_type_node) {
        db_.exec(
            "UPDATE edge_site SET recv_type_usr = NULL, recv_decl_usr = NULL");
      }
    }
    if (has_table("call_arg")) {
      add_col("call_arg", "src_kind_id", "INTEGER");
      add_col("call_arg", "type_id",
              "INTEGER REFERENCES type_node(id) ON DELETE SET NULL");
      add_col("call_arg", "decl_id",
              "INTEGER REFERENCES symbol(id) ON DELETE SET NULL");
      add_col("call_arg", "callee_id",
              "INTEGER REFERENCES symbol(id) ON DELETE SET NULL");
      add_col("call_arg", "type_identity_id",
              "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL");
      add_col("call_arg", "decl_identity_id",
              "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL");
      add_col("call_arg", "callee_identity_id",
              "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL");
      add_col("call_arg", "type_usr", "TEXT");
      add_col("call_arg", "decl_usr", "TEXT");
      add_col("call_arg", "callee_usr", "TEXT");
      add_col("call_arg", "type_is_value", "INTEGER");
      bool source_kind_not_null = false;
      {
        auto info = db_.prepare("PRAGMA table_info(call_arg)");
        while (info.step()) {
          if (info.col_text(1) == "src_kind") {
            source_kind_not_null = info.col_int64(3) != 0;
          }
        }
      }
      if (source_kind_not_null) {
        db_.exec("DROP VIEW IF EXISTS call_arg_read");
        db_.exec("PRAGMA foreign_keys = OFF");
        db_.exec(
            "CREATE TABLE call_arg_v35 ("
            "edge_id INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE, "
            "file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE, "
            "line INTEGER NOT NULL, col INTEGER NOT NULL, position INTEGER NOT "
            "NULL, "
            "src_kind TEXT, type_usr TEXT, decl_usr TEXT, callee_usr TEXT, "
            "src_kind_id INTEGER, type_id INTEGER REFERENCES type_node(id) ON "
            "DELETE SET NULL, "
            "decl_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL, "
            "callee_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL, "
            "type_identity_id INTEGER REFERENCES external_identity(id) ON "
            "DELETE SET NULL, "
            "decl_identity_id INTEGER REFERENCES external_identity(id) ON "
            "DELETE SET NULL, "
            "callee_identity_id INTEGER REFERENCES external_identity(id) ON "
            "DELETE SET NULL, "
            "type_is_value INTEGER, "
            "PRIMARY KEY(edge_id,file_id,line,col,position)) WITHOUT ROWID");
        db_.exec(
            "INSERT INTO call_arg_v35 SELECT edge_id,file_id,line,col,position,"
            "src_kind,type_usr,decl_usr,callee_usr,src_kind_id,type_id,decl_id,"
            "callee_id,type_identity_id,decl_identity_id,callee_identity_id,"
            "type_is_value FROM call_arg");
        db_.exec("DROP TABLE call_arg");
        db_.exec("ALTER TABLE call_arg_v35 RENAME TO call_arg");
        db_.exec("CREATE INDEX IF NOT EXISTS idx_call_arg_edge ON "
                 "call_arg(edge_id)");
        db_.exec("PRAGMA foreign_keys = ON");
      }
      if (has_call_identity_text && has_type_node) {
        db_.exec(
            "INSERT OR IGNORE INTO "
            "external_identity(identity_kind,identity_text,"
            "resolution_status,type_id) SELECT 1, ca.type_usr, "
            "CASE WHEN tn.id IS NULL THEN 0 ELSE 1 END, tn.id FROM call_arg ca "
            "LEFT JOIN type_node tn ON tn.decl_usr = ca.type_usr "
            "WHERE ca.type_usr IS NOT NULL");
      }
      if (has_call_identity_text && has_type_node) {
        db_.exec(
            "INSERT OR IGNORE INTO "
            "external_identity(identity_kind,identity_text,"
            "resolution_status,symbol_id) SELECT 2, u.value, "
            "CASE WHEN s.id IS NULL THEN 0 ELSE 1 END, s.id FROM "
            "(SELECT decl_usr AS value FROM call_arg UNION SELECT callee_usr "
            "FROM call_arg) u "
            "LEFT JOIN symbol s ON s.usr = u.value WHERE u.value IS NOT NULL");
        db_.exec(
            "UPDATE call_arg SET decl_id = (SELECT id FROM symbol s "
            "WHERE s.usr = call_arg.decl_usr), callee_id = (SELECT id FROM "
            "symbol s "
            "WHERE s.usr = call_arg.callee_usr), decl_identity_id = (SELECT id "
            "FROM external_identity i WHERE i.identity_kind = 2 AND "
            "i.identity_text = call_arg.decl_usr), callee_identity_id = "
            "(SELECT id "
            "FROM external_identity i WHERE i.identity_kind = 2 AND "
            "i.identity_text = call_arg.callee_usr)");
      }
      if (has_call_identity_text && has_type_node) {
        db_.exec(
            "UPDATE call_arg SET type_id = (SELECT id FROM type_node t WHERE "
            "t.decl_usr = call_arg.type_usr), type_identity_id = (SELECT id "
            "FROM external_identity i WHERE i.identity_kind = 1 AND "
            "i.identity_text = call_arg.type_usr)");
      }
      for (const auto &entry : catalog::kSourceKinds) {
        db_.exec(
            "UPDATE call_arg SET src_kind = NULL WHERE src_kind = 'value'");
        db_.exec(
            "UPDATE call_arg SET src_kind_id = " + std::to_string(entry.id) +
            " WHERE src_kind = '" + std::string(entry.name) + "'");
      }
      auto unknown = db_.prepare(
          "SELECT src_kind FROM call_arg WHERE src_kind IS NOT NULL "
          "AND src_kind_id IS NULL LIMIT 1");
      if (unknown.step()) {
        throw StorageError("unknown source kind '" + unknown.col_text(0) +
                           "' in call_arg migration");
      }
      db_.exec("UPDATE call_arg SET src_kind = NULL");
      if (has_call_identity_text && has_type_node) {
        db_.exec("UPDATE call_arg SET type_usr = NULL, decl_usr = NULL, "
                 "callee_usr = NULL");
      }
    }
    // ALTER TABLE cannot add the CHECK clauses that protect normalized enum
    // IDs. Rebuild both hot occurrence tables so migrated databases have the
    // same domains and foreign keys as a fresh v37 database.
    if (has_table("edge_site")) {
      add_col("edge_site", "args_sig", "TEXT");
      add_col("edge_site", "recv_param_pos", "INTEGER");
      add_col("edge_site", "recv_type_is_value", "INTEGER");
      db_.exec("DROP VIEW IF EXISTS edge_site_read");
      db_.exec("PRAGMA foreign_keys = OFF");
      db_.exec(
          "CREATE TABLE edge_site_v37 ("
          "edge_id INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE, "
          "file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE, "
          "line INTEGER, col INTEGER, conditional INTEGER NOT NULL DEFAULT 0, "
          "args_sig TEXT, recv_src_kind TEXT, recv_type_usr TEXT, "
          "recv_decl_usr TEXT, recv_src_kind_id INTEGER CHECK "
          "(recv_src_kind_id IS NULL OR recv_src_kind_id IN "
          "(1,2,3,4,5,6,7,8)), recv_type_id INTEGER REFERENCES type_node(id) "
          "ON DELETE SET NULL, recv_decl_id INTEGER REFERENCES symbol(id) ON "
          "DELETE SET NULL, recv_type_identity_id INTEGER REFERENCES "
          "external_identity(id) ON DELETE SET NULL, recv_decl_identity_id "
          "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL, "
          "recv_param_pos INTEGER, recv_type_is_value INTEGER, PRIMARY KEY "
          "(edge_id,file_id,line,col)) WITHOUT ROWID");
      db_.exec(
          "INSERT INTO edge_site_v37 SELECT edge_id,file_id,line,col,"
          "conditional,args_sig,NULL,NULL,NULL,recv_src_kind_id,recv_type_id,"
          "recv_decl_id,recv_type_identity_id,recv_decl_identity_id,"
          "recv_param_pos,recv_type_is_value FROM edge_site");
      db_.exec("DROP TABLE edge_site");
      db_.exec("ALTER TABLE edge_site_v37 RENAME TO edge_site");
      db_.exec("PRAGMA foreign_keys = ON");
    }
    if (has_table("call_arg")) {
      add_col("call_arg", "type_is_value", "INTEGER");
      db_.exec("DROP VIEW IF EXISTS call_arg_read");
      db_.exec("PRAGMA foreign_keys = OFF");
      db_.exec(
          "CREATE TABLE call_arg_v37 ("
          "edge_id INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE, "
          "file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE, "
          "line INTEGER NOT NULL, col INTEGER NOT NULL, position INTEGER NOT "
          "NULL, src_kind TEXT, type_usr TEXT, decl_usr TEXT, callee_usr TEXT, "
          "src_kind_id INTEGER CHECK (src_kind_id IS NULL OR src_kind_id IN "
          "(1,2,3,4,5,6,7,8)), type_id INTEGER REFERENCES type_node(id) ON "
          "DELETE SET NULL, decl_id INTEGER REFERENCES symbol(id) ON DELETE "
          "SET NULL, callee_id INTEGER REFERENCES symbol(id) ON DELETE SET "
          "NULL, type_identity_id INTEGER REFERENCES external_identity(id) ON "
          "DELETE SET NULL, decl_identity_id INTEGER REFERENCES "
          "external_identity "
          "(id) ON DELETE SET NULL, callee_identity_id INTEGER REFERENCES "
          "external_identity(id) ON DELETE SET NULL, type_is_value INTEGER, "
          "PRIMARY KEY(edge_id,file_id,line,col,position)) WITHOUT ROWID");
      db_.exec(
          "INSERT INTO call_arg_v37 SELECT edge_id,file_id,line,col,position,"
          "NULL,NULL,NULL,NULL,src_kind_id,type_id,decl_id,callee_id,"
          "type_identity_id,decl_identity_id,callee_identity_id,type_is_value "
          "FROM call_arg");
      db_.exec("DROP TABLE call_arg");
      db_.exec("ALTER TABLE call_arg_v37 RENAME TO call_arg");
      db_.exec(
          "CREATE INDEX IF NOT EXISTS idx_call_arg_edge ON call_arg(edge_id)");
      db_.exec("PRAGMA foreign_keys = ON");
    }
    if (has_table("symbol")) {
      add_col("symbol", "callable_kind", "TEXT");
      add_col("symbol", "template_origin", "TEXT");
      add_col("symbol", "template_form", "TEXT");
    }
    if (has_table("type_node")) {
      add_col("type_node", "extent", "TEXT");
    }
    changed = true;
  }
  if (changed) {
    auto st =
        db_.prepare("UPDATE meta SET value = ? WHERE key = 'schema_version'");
    st.bind(1, std::string_view(std::to_string(kSchemaVersion)));
    st.step_done();
  }
}

void SqliteStorageService::migrate_symbol_identity_scope() {
  db_.exec("DROP VIEW IF EXISTS edge_site_read");
  db_.exec("DROP VIEW IF EXISTS call_arg_read");
  db_.exec(
      "CREATE TABLE IF NOT EXISTS semantic_universe ("
      "id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, name TEXT NOT NULL, "
      "policy TEXT NOT NULL DEFAULT 'explicit');"
      "INSERT OR IGNORE INTO semantic_universe (id, key, name, policy) "
      "VALUES (1, 'legacy', 'Legacy single-workspace universe', 'legacy');");

  auto has_table = [this](const char *name) {
    auto st = db_.prepare(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
    st.bind(1, std::string_view(name));
    return st.step();
  };
  auto has_column = [this](const char *table, const char *column) {
    auto st = db_.prepare(std::string("PRAGMA table_info(") + table + ")");
    while (st.step()) {
      if (st.col_text(1) == column) {
        return true;
      }
    }
    return false;
  };
  if (has_table("repository") &&
      !has_column("repository", "semantic_universe_id")) {
    db_.exec("ALTER TABLE repository ADD COLUMN semantic_universe_id INTEGER "
             "NOT NULL DEFAULT 1");
  }
  if (has_table("component") &&
      !has_column("component", "semantic_universe_id")) {
    db_.exec("ALTER TABLE component ADD COLUMN semantic_universe_id INTEGER "
             "REFERENCES semantic_universe(id) ON DELETE SET NULL");
  }
  if (!has_table("symbol") || has_column("symbol", "semantic_universe_id")) {
    return;
  }

  const bool has_file_paths = has_table("file") && has_table("directory") &&
                              has_table("component") && has_table("repository");
  const std::string legacy_source =
      has_file_paths ? "COALESCE((SELECT 'source:' || "
                       "COALESCE(r.remote_url, 'repo:' || "
                       "r.name, 'component:' || c.path) || "
                       "char(31) || c.path || char(31) || "
                       "d.path || char(31) || f.name FROM "
                       "file f JOIN directory d ON d.id = "
                       "f.directory_id JOIN component c ON "
                       "c.id = d.component_id LEFT JOIN "
                       "repository r ON r.id = c.repository_id "
                       "WHERE f.id = symbol.file_id), "
                       "'source:unknown')"
                     : "'source:unknown'";

  db_.exec("PRAGMA foreign_keys = OFF");
  db_.exec(
      "CREATE TABLE symbol_v35 ("
      "id INTEGER PRIMARY KEY, usr TEXT NOT NULL, spelling TEXT NOT NULL, "
      "qual_name TEXT, display_name TEXT, kind INTEGER NOT NULL, "
      "type_info TEXT, file_id INTEGER REFERENCES file(id) ON DELETE SET NULL, "
      "line INTEGER, col INTEGER, end_line INTEGER, end_col INTEGER, "
      "decl_file_id INTEGER REFERENCES file(id) ON DELETE SET NULL, "
      "decl_line INTEGER, decl_col INTEGER, decl_path TEXT, "
      "is_definition INTEGER NOT NULL DEFAULT 0, "
      "is_pure INTEGER NOT NULL DEFAULT 0, is_static INTEGER NOT NULL DEFAULT "
      "0, "
      "is_instantiation INTEGER NOT NULL DEFAULT 0, "
      "is_named_instance INTEGER NOT NULL DEFAULT 0, linkage TEXT, access "
      "TEXT, "
      "parent_usr TEXT, resolved INTEGER NOT NULL DEFAULT 0, "
      "multi_def INTEGER NOT NULL DEFAULT 0, const_value TEXT, "
      "semantic_universe_id INTEGER NOT NULL DEFAULT 1 "
      "REFERENCES semantic_universe(id), identity_key TEXT NOT NULL DEFAULT "
      "'');");
  db_.exec(
      "INSERT INTO symbol_v35 (id, usr, spelling, qual_name, display_name, "
      "kind, type_info, file_id, line, col, end_line, end_col, decl_file_id, "
      "decl_line, decl_col, "
      "decl_path, is_definition, is_pure, is_static, is_instantiation, "
      "is_named_instance, linkage, access, parent_usr, resolved, multi_def, "
      "const_value, semantic_universe_id, identity_key) "
      "SELECT id, usr, spelling, qual_name, display_name, kind, type_info, "
      "file_id, line, col, end_line, end_col, decl_file_id, decl_line, "
      "decl_col, "
      "decl_path, is_definition, is_pure, is_static, is_instantiation, "
      "is_named_instance, linkage, access, parent_usr, resolved, multi_def, "
      "const_value, 1, "
      "'legacy' || char(31) || CASE WHEN linkage IN ('internal', 'no-linkage') "
      "THEN 'local:legacy' || char(31) || " +
      legacy_source +
      " || char(31) ELSE '' END || usr "
      "FROM symbol;");
  db_.exec("DROP TABLE symbol; ALTER TABLE symbol_v35 RENAME TO symbol;");
  db_.exec("PRAGMA foreign_keys = ON");
}

// v15 -> v16: rebuild `symbol` with kind stored as its CXCursorKind int.
// SQLite cannot ALTER a column's type or drop the old `kind IN (...)` CHECK, so
// the table is recreated and rows copied with kind names mapped to integers.
// Foreign keys are disabled for the swap so dropping the old table does not
// cascade-delete edges (edge.src_id/dst_id keep the ids the new rows carry).
// The schema script (run right after migrate) recreates the symbol indexes via
// CREATE INDEX IF NOT EXISTS. Mirrors storage.py _migrate_symbol_kind_to_int.
void SqliteStorageService::migrate_symbol_kind_to_int() {
  std::string cases;
  for (const auto &kv : symbol_kind_ids_map()) {
    cases += " WHEN '" + std::string(kv.first) + "' THEN " +
             std::to_string(kv.second);
  }
  db_.exec("PRAGMA foreign_keys = OFF");
  db_.exec("CREATE TABLE symbol_new ("
           " id INTEGER PRIMARY KEY,"
           " usr TEXT NOT NULL UNIQUE,"
           " spelling TEXT NOT NULL,"
           " qual_name TEXT,"
           " display_name TEXT,"
           " kind INTEGER NOT NULL,"
           " type_info TEXT,"
           " file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,"
           " line INTEGER,"
           " col INTEGER,"
           " decl_file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,"
           " decl_line INTEGER,"
           " decl_col INTEGER,"
           " decl_path TEXT,"
           " is_definition INTEGER NOT NULL DEFAULT 0,"
           " is_pure INTEGER NOT NULL DEFAULT 0,"
           " is_static INTEGER NOT NULL DEFAULT 0,"
           " is_instantiation INTEGER NOT NULL DEFAULT 0,"
           " linkage TEXT,"
           " access TEXT,"
           " parent_usr TEXT,"
           " resolved INTEGER NOT NULL DEFAULT 0"
           ");"
           "INSERT INTO symbol_new"
           " SELECT id, usr, spelling, qual_name, display_name,"
           "        CASE kind" +
           cases +
           " ELSE kind END,"
           "        type_info, file_id, line, col, decl_file_id, decl_line,"
           "        decl_col, decl_path, is_definition, is_pure, is_static,"
           "        is_instantiation, linkage, access, parent_usr, resolved"
           " FROM symbol;"
           "DROP TABLE symbol;"
           "ALTER TABLE symbol_new RENAME TO symbol;");
  db_.exec("PRAGMA foreign_keys = ON");
}

// v23 -> v24: rebuild `component` so `path` is UNIQUE per repository (was
// globally UNIQUE). A grouped component stores a clone-RELATIVE path, so
// several repositories can each carry a '.' root -- the old global UNIQUE(path)
// would reject that. Foreign keys are disabled for the swap so dropping the
// table does not cascade-delete directories (they keep the ids the copied rows
// carry). The schema script (run right after) is a no-op (CREATE TABLE IF NOT
// EXISTS). Mirrors Python _migrate_component_repo_unique.
void SqliteStorageService::migrate_component_repo_unique() {
  db_.exec("PRAGMA foreign_keys = OFF");
  db_.exec(
      "CREATE TABLE component_new ("
      " id      INTEGER PRIMARY KEY,"
      " name    TEXT NOT NULL,"
      " path    TEXT NOT NULL,"
      " kind    TEXT NOT NULL DEFAULT 'repo'"
      "         CHECK (kind IN ('repo', 'external')),"
      " version TEXT,"
      " repository_id INTEGER"
      "         REFERENCES repository(id) ON DELETE SET NULL,"
      " UNIQUE (repository_id, path)"
      ");"
      "INSERT INTO component_new"
      " SELECT id, name, path, kind, version, repository_id FROM component;"
      "DROP TABLE component;"
      "ALTER TABLE component_new RENAME TO component;");
  db_.exec("PRAGMA foreign_keys = ON");
}

// -- components
// ----------------------------------------------------------------

} // namespace cidx

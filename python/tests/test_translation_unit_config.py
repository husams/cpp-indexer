import sqlite3

from indexer.storage import (
    SCHEMA_VERSION,
    Storage,
    Symbol,
    FileConfigApplicability,
    TranslationUnitConfig,
    canonical_translation_unit_config_json,
    resolve_translation_unit_config,
    translation_unit_config_hash,
)


def test_descriptor_is_cross_language_stable_and_repeated_configs_dedupe():
    db = Storage(":memory:")
    config = resolve_translation_unit_config(
        ["-std=c++23", "-DFAST=1", "main.cpp"],
        driver="clang++", language="c++", diagnostics_policy="error-limit=0",
    )
    expected = (
        '["clang++","","c++","c++23","",[],"","",[],["-DFAST=1"],[],[],'
        '"error-limit=0",["-std=c++23","-DFAST=1","main.cpp"]]'
    )
    assert canonical_translation_unit_config_json(
        resolve_translation_unit_config(
            config.arguments,
            driver=config.driver,
            working_dir=config.working_dir,
            language=config.language,
            resource_dir=config.resource_dir,
        )
    ) == expected
    first = db.add_translation_unit_config(config)
    repeat = TranslationUnitConfig(
        driver="clang++", language="c++",
        arguments=["-std=c++23", "-DFAST=1", "main.cpp"],
        diagnostics_policy="error-limit=0",
    )
    assert db.add_translation_unit_config(repeat) == first

    changed = TranslationUnitConfig(
        driver="clang++", language="c++",
        arguments=["main.cpp", "-DFAST=1", "-std=c++23"],
        diagnostics_policy="error-limit=0",
    )
    assert db.add_translation_unit_config(changed) != first


def test_descriptor_golden_captures_every_semantic_dimension():
    config = TranslationUnitConfig(
        driver="clang++",
        working_dir=".",
        language="c++",
        resource_dir="/clang/resource",
        arguments=[
            "-std=c++23", "--target=x86_64-unknown-linux-gnu", "-mabi=lp64",
            "-isysroot", "/sdk", "-I", "/inc", "-D", "FEATURE=1",
            "-include", "/gen/header.hpp",
        ],
    )
    expected = (
        '["clang++",".","c++","c++23","x86_64-unknown-linux-gnu",'
        '["-mabi=lp64"],"/sdk","/clang/resource",["/inc"],'
        '["-DFEATURE=1"],[],["/gen/header.hpp"],"error-limit=0",'
        '["-std=c++23","--target=x86_64-unknown-linux-gnu","-mabi=lp64",'
        '"-isysroot","/sdk","-I","/inc","-D","FEATURE=1",'
        '"-include","/gen/header.hpp"]]'
    )
    assert canonical_translation_unit_config_json(
        resolve_translation_unit_config(
            config.arguments,
            driver=config.driver,
            working_dir=config.working_dir,
            language=config.language,
            resource_dir=config.resource_dir,
        )
    ) == expected
    assert translation_unit_config_hash(
        resolve_translation_unit_config(
            config.arguments,
            driver=config.driver,
            working_dir=config.working_dir,
            language=config.language,
            resource_dir=config.resource_dir,
        )
    ) == "0e65af5d6defe83a2ea53aeac13ca9f6237c4a20"


def test_v34_include_row_migrates_to_shared_config_identity(tmp_path):
    path = tmp_path / "v34.db"
    db = Storage(str(path))
    component = db.add_component("config", "/repo/config")
    directory = db.add_directory(component, "")
    tu = db.add_file(directory, "main.cpp")
    db.close()

    raw = sqlite3.connect(path)
    for table in (
        "include_macro_use", "include_site", "include_edge", "include_config",
        "file_config", "translation_unit", "translation_unit_config",
    ):
        raw.execute(f"DROP TABLE {table}")
    raw.execute(
        "CREATE TABLE include_config (id INTEGER PRIMARY KEY, tu_file_id INTEGER NOT NULL, "
        "digest TEXT NOT NULL, driver TEXT, working_dir TEXT, arguments TEXT, "
        "lang_mode TEXT, resource_dir TEXT, UNIQUE(tu_file_id, digest))"
    )
    raw.execute(
        "INSERT INTO include_config(tu_file_id, digest, driver, working_dir, arguments, lang_mode) "
        "VALUES (?, 'legacy', 'clang++', '.', ?, 'c++')",
        (tu, '["-std=c++23","main.cpp"]'),
    )
    raw.execute("UPDATE meta SET value = '34' WHERE key = 'schema_version'")
    raw.commit()
    raw.close()

    migrated = Storage(str(path))
    assert migrated._conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0] == str(SCHEMA_VERSION)
    assert migrated._conn.execute(
        "SELECT COUNT(*) FROM translation_unit_config"
    ).fetchone()[0] == 1
    assert migrated._conn.execute(
        "SELECT translation_unit_config_id FROM include_config"
    ).fetchone()[0] is not None
    assert len(migrated.translation_unit_configs_for_file(tu)) == 1


def test_configuration_change_invalidates_only_the_tu_products():
    db = Storage(":memory:")
    component = db.add_component("config", "/repo/config")
    directory = db.add_directory(component, "")
    tu = db.add_file(directory, "main.cpp")
    config = db.add_translation_unit_config(TranslationUnitConfig(arguments=["main.cpp"]))
    db._conn.execute(
        "INSERT INTO translation_unit(file_id, config_id) VALUES (?, ?)",
        (tu, config),
    )
    db.add_file_config(FileConfigApplicability(tu, config, role="translation_unit"))
    db.set_file_indexed(tu, True)
    db.add_file(directory, "main.cpp", compile_options=["-DCHANGED"], driver="clang++")
    assert db.get_file_by_id(tu).indexed is False
    assert db.file_configs_for(tu)[0].state == "stale"


def test_header_can_be_applicable_under_multiple_typed_states():
    db = Storage(":memory:")
    component = db.add_component("config", "/repo/config")
    directory = db.add_directory(component, "")
    header = db.add_file(directory, "shared.hpp")
    first = db.add_translation_unit_config(TranslationUnitConfig(arguments=["a.cpp"]))
    second = db.add_translation_unit_config(TranslationUnitConfig(arguments=["b.cpp"]))
    db.add_file_config(FileConfigApplicability(header, first))
    db.add_file_config(FileConfigApplicability(header, second, state="ambiguous", reason="different configs"))
    rows = db.file_configs_for(header)
    assert [(r.config_id, r.state) for r in rows] == [(first, "registered"), (second, "ambiguous")]
    assert db.invariant_include_edges(header, [first, second]).coverage_complete is False
    db.add_file_config(FileConfigApplicability(header, second, state="registered"))
    invariant = db.invariant_include_edges(header, [first, second])
    assert invariant.coverage_complete is True
    assert invariant.edges == []


def test_configuration_qualified_symbol_reads_have_all_invariant_unknown_views():
    db = Storage(":memory:")
    component = db.add_component("config", "/repo/config")
    directory = db.add_directory(component, "")
    header = db.add_file(directory, "conditional.hpp")
    first = db.add_translation_unit_config(TranslationUnitConfig(arguments=["a.cpp"]))
    second = db.add_translation_unit_config(TranslationUnitConfig(arguments=["b.cpp"]))
    db.add_file_config(FileConfigApplicability(header, first))
    db.add_file_config(FileConfigApplicability(header, second))
    one = db.add_symbol(Symbol("usr-a", "only_a", "function", qual_name="only_a"))
    two = db.add_symbol(Symbol("usr-b", "only_b", "function", qual_name="only_b"))
    db._conn.executemany(
        "INSERT INTO fact_applicability(fact_kind, fact_id, file_id, config_id) "
        "VALUES ('symbol', ?, ?, ?)",
        [(one, header, first), (two, header, second)],
    )
    db._conn.commit()
    assert [s.usr for s in db.symbols_for_config(header, [first], "one").symbols] == [
        "usr-a"
    ]
    assert {s.usr for s in db.symbols_for_config(header, [first, second], "all").symbols} == {
        "usr-a", "usr-b"
    }
    assert db.symbols_for_config(header, [first, second], "invariant").symbols == []
    assert not db.symbols_for_config(header, [first, 999], "one").coverage_complete

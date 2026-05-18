---
run_id: cpp-indexer-m8-v2
milestone: M8 — Structured Node Attributes
author: business-analyst
created: 2026-05-18
source: /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements.md
---

# Requirements

## In-scope

- S40: `SCHEMA_VERSION` bump, `NodeRecord`/`EdgeRecord` new promoted fields, Arrow round-trip.
- S41: Callable extraction — `return_type`, `params`, `signature`, bounded `code` / `code_truncated`.
- S42: Template parameter and specialization argument extraction.
- S43: USES edge access classifier (`source_association_type`, `target_association_type`).
- S44: Neo4j native property writes and covering indexes.
- S45: IndraDB native property writes and sink parity with Neo4j.
- S46: Schema docs, prompt/example refresh, wiki cross-links.

## Out-of-scope

- New node/edge kinds beyond those specified.
- NL-to-Cypher translator.
- Automatic migration of old-schema graphs.
- `exception_spec`, `control_flow`, `bit_field` promotion (remain in `attrs_json`).
- `EXTERNAL_REF` USES classification mirroring (scope unresolved — see Open questions).

## Assumptions

1. (assumed) `SCHEMA_VERSION` is a single integer or semver string constant with a single canonical location that the schema-handshake test reads.
2. (assumed) The 32 KiB boundary in AC-S41-5/6 is computed on the raw UTF-8 byte length of the source span returned by `entity.get_range()`.
3. (assumed) Exactly the 7 values listed in AC-S43-1 (`read`, `write`, `addr_of`, `call_arg`, `return`, `decl_ref`, `unknown`) are the complete closed enumeration; no others are permitted to reach the sink.
4. (assumed) "non-null values" in AC-S41-7 means the Cypher property returns a string, not `null` or an empty string.
5. (assumed) Regression scenarios for existing ignored tests (AC-S44-5, AC-S45-4) are exercised by the existing test suite; BDD scenarios here assert the expected outcome, not test mechanics.
6. (assumed) The `code` field stores the verbatim source bytes; the `code_truncated: true` path stores `None` in `NodeRecord.code`, not a truncated prefix.

## Open questions

1. **OQ-1** [needs-clarification] AC-S40-2 / AC-S44-1: Are `is_virtual`, `is_pure_virtual`, `is_static` fully promoted out of `attrs_json`, or dual-written during a transition window? PRD implies full promotion (indexes on them at S44), but AC-S3.1 in the PRD does not list them explicitly. Architect must confirm before S40 developer dispatch.
2. **OQ-2** [needs-clarification] AC-S41-5/6: Is `code` stored on the node (current AC text) or in a sidecar blob keyed by `usr`? If sidecar wins, AC-S41-5 and AC-S41-6 — and the scenarios `SCN-S41-CODE-WITHIN-LIMIT` and `SCN-S41-CODE-EXCEEDS-LIMIT` — must be revised before developer dispatch.
3. **OQ-3** [needs-clarification] S43: Does the cross-repo `EXTERNAL_REF` edge require USES classification mirrored on it (Phase 5)? Scope must be set before S43 developer dispatch; if in scope, new scenarios are required.
4. **OQ-4** [needs-clarification] S44: Is the Neo4j deadlock retry fix (`Neo.TransientError.Transaction.DeadlockDetected`) a prerequisite for S44, a parallel track, or a follow-up? If prerequisite, S44 developer dispatch is blocked.
5. **OQ-5** [needs-clarification] S45: IndraDB v5 has limited property-index control. Should any index-equivalent behavior be attempted now, or documented as a known limitation for v6?

## Edge cases

- 32 KiB exactly: code fits, `code_truncated: false`. (confirmed — AC-S41-5 says ≤)
- 32 KiB + 1 byte: `code: None`, `code_truncated: true`; must not panic. (confirmed — AC-S41-6)
- Old `SCHEMA_VERSION` at handshake: `SchemaVersionMismatch` error, not a panic or generic I/O error. (confirmed — AC-S40-5)
- Access classifier fallback: unrecognized context → `unknown`, log entry written, edge NOT dropped. (confirmed — AC-S43-3)
- No eighth access-mode value escapes to the sink. (confirmed — AC-S43-1)
- Promoted field not double-written to `attrs_json`. (confirmed — AC-S40-6)
- `code_truncated: true` on IndraDB: node write succeeds, no test failure. (confirmed — AC-S45-5)
- Arrow round-trip for `None` (absent optional field): no data loss. (confirmed — AC-S40-4)
- `SHOW INDEXES` composite index present after fresh write. (confirmed — AC-S44-3)
- `EXPLAIN` plan returns `NodeIndexSeek`, not `AllNodesScan` or `NodeByLabelScan`. (confirmed — AC-S44-4)

## Stakeholders

- cpp-indexer developers (S40–S46 implementers)
- Agent consumers (Cypher + IndraDB query writers, Q1–Q5 from PRD §6)
- QA engineer (gated Neo4j / IndraDB integration tests)
- Devops (re-index runbook, AC-S46-2)
- Architect (ADR-9 schema version policy; deadlock retry decision; OQ-1 through OQ-5)

---

# Gherkin

## Feature: S40 — Schema version, NodeRecord/EdgeRecord fields, Arrow round trips

```gherkin
Feature: S40 Schema version bump and structured record fields

  @AC-S40-1
  Scenario: Handshake rejects old schema version
    Given a graph written with the previous SCHEMA_VERSION constant
    When the schema-handshake test (m6_agent_gate) runs against that graph
    Then the test fails, confirming old and new schema versions cannot coexist

  @AC-S40-2
  Scenario: NodeRecord carries all promoted optional fields
    Given the codebase after S40 lands
    When the NodeRecord struct in src/schema/ is inspected
    Then it contains fields: return_type, params, signature, code, code_truncated,
         template_params, template_args, is_virtual, is_pure_virtual, is_static
    And every field is typed Option<T> as specified in AC-S40-2

  @AC-S40-3
  Scenario: EdgeRecord carries association type fields
    Given the codebase after S40 lands
    When the EdgeRecord struct in src/schema/ is inspected
    Then it contains fields source_association_type and target_association_type
    And each field is typed Option<String>

  @AC-S40-4
  Scenario Outline: Arrow round-trip preserves new fields for Some and None
    Given a NodeRecord or EdgeRecord with <field> set to <value>
    When the record is serialized to Arrow format and deserialized back
    Then <field> equals <value> with no data loss
    And no panic or serialization error occurs

    Examples:
      | field              | value           |
      | return_type        | Some("int")     |
      | return_type        | None            |
      | params             | Some([{n,t}])   |
      | params             | None            |
      | code_truncated     | Some(true)      |
      | code_truncated     | Some(false)     |
      | code_truncated     | None            |
      | source_association_type | Some("read") |
      | source_association_type | None        |

  @AC-S40-5
  Scenario: Schema version mismatch surfaces explicit error
    Given a graph written by the previous schema version
    When the schema handshake runs against that graph
    Then the system returns a SchemaVersionMismatch error
    And the error is not a panic
    And the error is not a generic I/O error

  @AC-S40-6
  Scenario: Promoted fields are not double-written to attrs_json
    Given a node record with a promoted field (e.g. return_type) set to a value
    When the record is written to either sink
    Then return_type appears as a native property
    And return_type does not appear inside attrs_json
    And attrs_json retains non-promoted fields: exception_spec, control_flow, bit_field
```

---

## Feature: S41 — Callable extraction: return type, params, signature, bounded code

```gherkin
Feature: S41 Callable attribute extraction for FUNCTION and METHOD nodes

  @AC-S41-1
  Scenario: return_type populated for FUNCTION and METHOD
    Given a FUNCTION or METHOD cursor in src/visit/shallow.rs
    When the visitor runs on that cursor
    Then NodeRecord.return_type is set to entity.get_result_type().get_display_name()
    And the value is non-empty for a cursor with a known return type

  @AC-S41-2
  Scenario: params populated as ordered name/type list
    Given a FUNCTION or METHOD cursor with two or more parameters
    When the visitor runs on that cursor
    Then NodeRecord.params is an ordered list of {name, type} pairs
    And each pair is populated via arg.get_name() and arg.get_type().get_display_name()

  @AC-S41-3 @AC-S41-4
  Scenario Outline: signature constructed correctly for method and free function
    Given a <kind> cursor with return type <ret_type> and param types <param_types>
      And cv-qualifier <cv> where applicable
    When the visitor constructs the signature
    Then NodeRecord.signature equals <expected_signature>

    Examples:
      | kind     | ret_type | param_types | cv    | expected_signature              |
      | METHOD   | int      | (int, bool) | const | int(int, bool) const            |
      | METHOD   | void     | ()          | none  | void()                          |
      | FUNCTION | int      | (int)       | n/a   | int(int)                        |
      | FUNCTION | void     | ()          | n/a   | void()                          |

  @AC-S41-5
  Scenario: code populated when source range is at or below 32 KiB
    Given a FUNCTION or METHOD cursor whose source range byte length is exactly 32768 bytes
    When the visitor reads entity.get_range() from file_path
    Then NodeRecord.code is populated with the verbatim source span
    And NodeRecord.code_truncated is false

  @AC-S41-6
  Scenario: code set to None when source range exceeds 32 KiB
    Given a FUNCTION or METHOD cursor whose source range byte length is 32769 bytes or more
    When the visitor reads the source span
    Then NodeRecord.code is None
    And NodeRecord.code_truncated is true
    And no error or panic occurs

  @AC-S41-7
  Scenario: leveldb::DBImpl::Open has all callable fields populated in Neo4j
    Given the leveldb corpus has been indexed with S41 in place
    When the Cypher query is executed:
      MATCH (m:Node {qualified_name: 'leveldb::DBImpl::Open'})
      RETURN m.return_type, m.signature, m.params
    Then m.return_type is non-null
    And m.signature is non-null
    And m.params is non-null

  @AC-S41-8
  Scenario: Unit tests assert callable fields for known fixture functions
    Given the S41 unit test suite on fixture files
    When the tests run
    Then at least one test asserts return_type for a known fixture function
    And at least one test asserts params for a known fixture function
    And at least one test asserts signature for a known fixture function
    And at least one test asserts the same three fields for a known fixture method
```

---

## Feature: S42 — Template parameter and specialization argument extraction

```gherkin
Feature: S42 Template extraction for TEMPLATE_DECL and SPECIALIZATION nodes

  @AC-S42-1
  Scenario: template_params populated for TEMPLATE_DECL
    Given a TEMPLATE_DECL cursor with type, non-type, and template-template parameters
    When the visitor runs on that cursor
    Then NodeRecord.template_params is a list of {name, kind, default?} entries
    And the list includes entries from TemplateTypeParameter, NonTypeTemplateParameter,
        and TemplateTemplateParameter child cursors

  @AC-S42-2
  Scenario: template_args populated as structured list for SPECIALIZATION
    Given a SPECIALIZATION cursor whose arguments were previously stored as a debug string
    When the visitor runs on that cursor
    Then NodeRecord.template_args is a list of {kind, value} entries
    And no debug-printed string form appears in template_args
    And attrs_json does not contain the old debug-string form of template_args

  @AC-S42-3
  Scenario: leveldb std::vector specialization has structured template_args in Neo4j
    Given the leveldb corpus has been indexed with S42 in place
    When the Cypher query is executed:
      MATCH (s:Node {kind:'SPECIALIZATION'})-[:SPECIALIZES]->(:Node {qualified_name:'std::vector'})
      RETURN s.template_args
    Then template_args returns a non-empty structured list
    And the returned value is not a debug string

  @AC-S42-4
  Scenario: Unit tests assert template fields for known fixture declarations
    Given the S42 unit test suite on fixture files
    When the tests run
    Then at least one test asserts correct template_params for a known template declaration
    And at least one test asserts correct template_args for a known specialization
```

---

## Feature: S43 — USES edge access classifier

```gherkin
Feature: S43 USES edge source and target association type classification

  @AC-S43-1
  Scenario: source_association_type values are restricted to the closed enumeration
    Given a USES edge emitted by the visitor for any cursor context
    When source_association_type is set
    Then its value is one of exactly: read, write, addr_of, call_arg, return, decl_ref, unknown
    And no other value is permitted to reach the sink

  @AC-S43-2
  Scenario: target_association_type reflects referenced entity kind symmetrically
    Given a USES edge where the target is a field used as a write target
    When source_association_type is set to write
    Then target_association_type is also set to write

  @AC-S43-3
  Scenario: unclassifiable cursor context falls back to unknown without dropping edge
    Given a USES edge emission site where the cursor context is an overloaded operator
      or a pointer dereference write that cannot be confidently categorized
    When the classifier runs
    Then source_association_type is set to unknown
    And a log entry is written describing the unclassified context
    And the edge is emitted to the sink (not dropped)

  @AC-S43-4
  Scenario: association type fields written as native edge properties
    Given a USES edge with source_association_type and target_association_type set
    When the edge is written to either sink
    Then source_association_type and target_association_type appear as native edge properties
    And neither field appears inside attrs_json

  @AC-S43-5
  Scenario: leveldb mutex_ USES edges have non-zero access-mode buckets in Neo4j
    Given the leveldb corpus has been indexed with S43 in place
    When the Cypher query is executed:
      MATCH (m:Node {kind:'METHOD'})-[u:EDGE {kind:'USES'}]->(:Node {qualified_name:'leveldb::DBImpl::mutex_'})
      RETURN u.source_association_type, count(*) AS n
    Then the result contains at least one row with a non-zero count per source_association_type bucket

  @AC-S43-6
  Scenario Outline: classifier unit tests cover required access modes
    Given a fixture file containing a <context> usage pattern
    When the S43 classifier unit tests run on that fixture
    Then the emitted USES edge has source_association_type equal to <expected_type>

    Examples:
      | context                                 | expected_type |
      | plain variable read                     | read          |
      | assignment to field                     | write         |
      | variable passed as function argument    | call_arg      |
      | overloaded operator (unclassifiable)    | unknown       |
```

---

## Feature: S44 — Neo4j native property writes and indexes

```gherkin
Feature: S44 Neo4j sink writes promoted properties and creates covering indexes

  @AC-S44-1
  Scenario: Promoted node properties written as top-level Cypher properties
    Given a NodeRecord with promoted fields populated (return_type, params, signature,
          code, code_truncated, template_params, template_args, is_virtual,
          is_pure_virtual, is_static)
    When the Neo4j sink writes the node via CQL_MERGE_NODES
    Then all promoted fields appear as top-level Cypher node properties
    And none of the promoted fields are absent from the node in Neo4j

  @AC-S44-2
  Scenario: Promoted edge properties written as native Cypher edge properties
    Given a USES EdgeRecord with source_association_type and target_association_type set
    When the Neo4j sink writes the edge via CQL_MERGE_EDGES
    Then source_association_type and target_association_type appear as native edge properties
    And neither field is absent from the edge in Neo4j

  @AC-S44-3
  Scenario Outline: Required indexes exist after fresh Neo4j write
    Given a fresh Neo4j instance after indexing with S44 in place
    When SHOW INDEXES is run
    Then the index <index_name> exists on <property> for label Node

    Examples:
      | index_name                  | property                   |
      | node_return_type_idx        | return_type                |
      | node_is_virtual_idx         | is_virtual                 |
      | node_is_static_idx          | is_static                  |
      | node_kind_return_type_idx   | (kind, return_type)        |

  @AC-S44-3
  Scenario: Indexes created with IF NOT EXISTS (idempotent)
    Given a Neo4j instance where the indexes already exist
    When the sink initialization runs again
    Then no error occurs due to duplicate index creation

  @AC-S44-4
  Scenario: Query plan uses NodeIndexSeek for indexed predicates
    Given a Neo4j instance with S44 indexes in place and data written
    When EXPLAIN is run for:
      MATCH (m:Node {kind:'METHOD', is_virtual:true})
      WHERE m.return_type = 'leveldb::Status'
      RETURN m.qualified_name
    Then the explain plan reports NodeIndexSeek for the indexed predicates
    And the plan does not report AllNodesScan
    And the plan does not report NodeByLabelScan

  @AC-S44-5
  Scenario: Existing ignored Neo4j tests continue to pass after S44 lands
    Given the 3 previously-ignored Neo4j integration tests
    When S44 lands and those tests are run
    Then all 3 tests pass with no regression
```

---

## Feature: S45 — IndraDB native property writes

```gherkin
Feature: S45 IndraDB sink writes promoted properties as distinct vertex and edge property keys

  @AC-S45-1
  Scenario: Promoted node fields written as separate vertex property keys
    Given a NodeRecord with promoted fields populated
    When the IndraDB sink writes the node
    Then each promoted field (return_type, params, signature, code, code_truncated,
         template_params, template_args, is_virtual, is_pure_virtual, is_static)
         appears as a distinct vertex property key
    And no promoted field is folded into a single JSON blob

  @AC-S45-2
  Scenario: Association type fields written as distinct edge property keys
    Given a USES EdgeRecord with source_association_type and target_association_type set
    When the IndraDB sink writes the edge
    When edge properties are read back via get_edge_properties
    Then source_association_type appears as a distinct edge property key
    And target_association_type appears as a distinct edge property key

  @AC-S45-3
  Scenario: Integration test verifies return_type, params, signature for a fixture METHOD node
    Given a gated integration test using get_vertex_properties on the IndraDB memory backend
    When a known FUNCTION or METHOD fixture node is queried
    Then the response includes key return_type with a non-null value
    And the response includes key params with a non-null value
    And the response includes key signature with a non-null value

  @AC-S45-4
  Scenario: Existing ignored IndraDB tests continue to pass after S45 lands
    Given the 12 previously-ignored IndraDB integration tests
    When S45 lands and those tests are run
    Then all 12 tests pass with no regression

  @AC-S45-5
  Scenario: Node with code_truncated true does not cause test failure in IndraDB
    Given a FUNCTION node whose source range exceeds 32 KiB (code_truncated is true, code is None)
    When the IndraDB sink writes that node
    Then no test failure or panic occurs
    And the truncation code path is exercised by at least one test

  @AC-S45-1 @AC-S44-1
  Scenario: Sink parity — promoted node properties match between Neo4j and IndraDB for same fixture
    Given a known fixture FUNCTION node indexed through both sinks
    When the node is read back from Neo4j as top-level Cypher properties
      And the node is read back from IndraDB via get_vertex_properties
    Then the set of promoted property keys is identical in both sinks
    And no promoted property is present in one sink but absent in the other

  @AC-S45-2 @AC-S44-2
  Scenario: Sink parity — promoted edge properties match between Neo4j and IndraDB for same fixture
    Given a known fixture USES edge indexed through both sinks
    When the edge is read back from Neo4j
      And the edge is read back from IndraDB via get_edge_properties
    Then source_association_type and target_association_type are present in both sinks
```

---

## Feature: S46 — Schema docs, prompt/example refresh, wiki cross-links

```gherkin
Feature: S46 Schema documentation, prompt updates, and wiki cross-links

  @AC-S46-1
  Scenario: SCHEMA.md lists every promoted property with required metadata
    Given docs/schema/SCHEMA.md written as part of S46
    When the file is inspected
    Then it lists every promoted property (return_type, params, signature, code,
         code_truncated, template_params, template_args, is_virtual, is_pure_virtual,
         is_static, source_association_type, target_association_type)
    And for each property it includes: field name, type, node/edge kinds that carry it,
        source-of-truth code path, and a sample Cypher query

  @AC-S46-2
  Scenario: Staging recovery runbook includes re-index recipe
    Given docs/runbooks/staging-recovery.md updated as part of S46
    When the file is inspected
    Then it includes a re-index recipe stating old-schema graphs must be wiped and re-indexed
    And it includes the exact CLI command sequence to perform a fresh index

  @AC-S46-3
  Scenario: Wiki cpp-indexer page references M8 promoted properties
    Given the wiki page pages/code/cpp-indexer updated as part of S46
    When the page is inspected
    Then it references the M8 promoted properties
    And it links to pages/planning/cpp-indexer-structured-attrs-prd

  @AC-S46-4
  Scenario: Schema prompt and examples include Q1 and Q5 queries
    Given prompt/graph_database/cpp/schema.txt updated as part of S46
    When the file is inspected
    Then it includes a worked example demonstrating Q1 (return_type / signature query)
    And it includes a worked example demonstrating Q5 (source_association_type query)

  @AC-S46-5
  Scenario: Field-name drift between schema.txt, SCHEMA.md, and sink writes is detectable
    Given schema.txt, docs/schema/SCHEMA.md, and the sink write code all land as part of S46
    When a drift-check step (manual checklist or automated test) is executed
    Then any discrepancy in a promoted field name between schema.txt, SCHEMA.md,
         and a live sink query is surfaced
    And the check does not silently pass when a name mismatch exists
```

---

# References

- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements.md`
- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/CHARTER.md`
- `[[pages/code/cpp-indexer]]`
- `[[pages/planning/cpp-indexer-structured-attrs-prd]]` (cited in requirements.md)
- `[[pages/planning/cpp-indexer-structured-attrs-brief]]` (cited in requirements.md)
- Cognee node-sets: `task:cpp-indexer-m8`, `role:business-analyst`

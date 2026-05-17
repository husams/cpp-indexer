# M6 Agent Gate — Manual Procedure

Story: S32-m6-agent-gate  
AC covered: AC-M6-8, AC-M6-9  
Status: **MANUAL RUN REQUIRED** — results must be recorded in `test-report.md`

---

## Purpose

Verify that the CodexGraph Streamlit agent (cpp-mcp), configured with the
`prompt/graph_database/cpp/schema.txt` from this repo, can correctly answer
natural-language questions about an LLVM-indexed graph.

The schema handshake contract tested here is defined in ADR-1:

- `cpp-indexer` is the single source of truth for `schema.txt` and `SCHEMA_VERSION`.
- `cpp-mcp` vendors the artifact from a tagged cpp-indexer release.
- At agent startup, cpp-mcp queries the live graph's `SchemaVersion` node and
  refuses if the version differs from the embedded `schema.txt` version comment.

---

## Prerequisites

1. An LLVM source checkout indexed with `cxg-index`:

   ```sh
   cxg-index /path/to/llvm-project/llvm \
       --repo-name llvm \
       --neo4j-uri bolt://localhost:7687 \
       --neo4j-password <password>
   ```

2. The CodexGraph Streamlit agent (cpp-mcp) running locally or remotely,
   configured with the schema from this repo's latest tag:

   ```sh
   # Inside the cpp-mcp repo:
   cp /path/to/cpp-indexer/prompt/graph_database/cpp/schema.txt \
       prompt/graph_database/cpp/schema.txt
   cp /path/to/cpp-indexer/prompt/graph_database/cpp/example.txt \
       prompt/graph_database/cpp/example.txt
   streamlit run app.py
   ```

3. The agent must connect to the same Neo4j instance that holds the indexed
   LLVM graph.

4. Verify the handshake: on startup, cpp-mcp logs the `SchemaVersion` tag it
   found in the graph. Confirm it matches the version comment on line 3 of
   `prompt/graph_database/cpp/schema.txt`:

   ```
   # schema-version: cxg-schema-v4
   ```

   If they differ, the agent will refuse queries — re-index with the current
   binary before continuing.

---

## Test questions

Execute each question from `tests/integration/m6_nl_eval.json` in the
Streamlit agent chat interface. For each question, record:

- The full agent response (paste into test-report.md or attach as a file).
- Your grade: **PASS** if the response contains at least one of the
  `expected_fragments`; **FAIL** otherwise.

See `tests/integration/m6_nl_eval.json` for the 10 questions and their
expected answer fragments.

---

## Pass criterion

**AC-M6-8:** Q1 ("What classes inherit from `llvm::Value`?") must return a
non-empty answer referencing at least one real LLVM class (e.g. `User`,
`Argument`, `BasicBlock`).

**AC-M6-9:** At least 8 out of 10 questions graded PASS.

---

## Recording results

Record pass/fail for each question, plus the final score, in `test-report.md`
under the S32 section. Tag unresolved failures as `QA_DEFECT` per the CHARTER
taxonomy.

---

## Schema contract note (for cpp-mcp maintainers)

The version comment embedded in `schema.txt` line 3 is the machine-readable
handshake token. cpp-mcp MUST parse this line to derive the expected schema
version before querying the graph's `SchemaVersion` node. The format is:

```
# schema-version: <SCHEMA_VERSION_TAG>
```

where `<SCHEMA_VERSION_TAG>` matches `src/schema/version.rs` constant
`SCHEMA_VERSION_TAG`. This string is the single source of truth; do not
regenerate or override it in cpp-mcp.

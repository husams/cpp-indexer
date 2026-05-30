---
name: codebase-semantic-analyzer
description: Semantic analysis and navigation of the cpp-indexer codebase, avoiding unstructured file reads.
allowed-tools: [grep_search, run_command]
disable-model-invocation: false
---

# Codebase Semantic Analyzer Skill

This skill enforces a systematic, semantic navigation protocol for exploring the `cpp-indexer` codebase. Instead of doing unstructured, blind file reads (`view_file`), this skill mandates utilizing structural parser scripts and graph database queries to build a semantic understanding of symbol relationships.

## Operating Guidelines

### 1. Prohibition of Unstructured Reads
* Do **NOT** use `view_file` to read entire source files blindly for code exploration or analysis.
* You may only read specific line ranges in a file after:
  1. Locating the exact file and lines via `grep_search`.
  2. Running the structural parser (`rust_parser.py`) to determine line boundaries for target definitions.

### 2. Semantic Analysis Workflow
* **Discovery Phase**: Use `grep_search` to find symbols, structure names, or module references.
* **Structural Parsing**: Run `/Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/scripts/rust_parser.py` on the candidate file to identify starting/ending line boundaries for functions, structs, impl blocks, and traits.
* **Graph Querying**: For C++ parser relationships (e.g. override chains, call graphs, module inclusions), execute `/Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/scripts/query_graph.py` with custom Cypher queries against the Neo4j database.

## Provided Tools & Scripts

### Rust Structural Parser
Run this python script to extract struct, enum, trait, function, and impl blocks along with line numbers:
```bash
python3 /Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/scripts/rust_parser.py <path_to_rust_file>
```

### C++ AST Parser
Run this python script to dump C++ AST declarations (namespaces, classes, methods, functions, variables) parsed via clang:
```bash
python3 /Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/scripts/ast_analyzer.py <path_to_cpp_file>
```

### Graph database Cypher client
Run this client to query the live code graph directly:
```bash
python3 /Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/scripts/query_graph.py "<cypher_query>"
```
*Example:*
```bash
python3 /Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/scripts/query_graph.py "MATCH (n:Node) RETURN count(n) AS node_count"
```

## Extending the Skill Library

Agents are strongly encouraged to extend the analysis capabilities by writing reusable scripts and placing them in the skill's library directory:
`/Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/lib/`

When writing extensions:
1. Make them generic (e.g. tracing rust imports, generating mermaid diagrams from graph query results, mapping file changes to semantic definitions).
2. Save them to `/Users/husam/workspace/cpp-indexer/.skills/codebase-semantic-analyzer/lib/`.
3. Add a section documenting the script, its inputs/outputs, and usage pattern to this `SKILL.md` file.

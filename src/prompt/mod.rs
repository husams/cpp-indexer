//! MCP schema-prompt generation (AC-M6-1, AC-M6-2).
//!
//! `codegen::generate_schema` is the single source of truth for
//! `prompt/graph_database/cpp/schema.txt`. The file is written at build time
//! by `build.rs` and must not be hand-edited.

pub mod codegen;

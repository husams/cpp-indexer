// HeaderStats — the {indexed, symbols, already, system, unowned} counters
// returned by the per-TU header-indexing pass (ast.py:203). Plain data and
// clang-free, so both the LibTooling engine and the CLI can share it without
// pulling in any Clang API. Relocated here from the retired src/clangx/ast.hpp
// when the libclang cursor-walk engine was removed.
#pragma once

namespace cidx {

struct HeaderStats {
  int indexed = 0; // headers indexed by this call
  int symbols = 0; // symbols stored while indexing them
  int already = 0; // skipped: file row indexed with matching md5
  int system = 0;  // skipped: system header (default policy)
  int unowned = 0; // skipped: no registered component owns the path
};

} // namespace cidx

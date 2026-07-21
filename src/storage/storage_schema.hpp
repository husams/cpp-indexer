// The v31 schema script and the v2 -> v3 qual_name backfill, split out of
// storage.cpp so the SQL text lives in one place instead of dominating the
// connection logic. The text is frozen: every existing index.db depends on it.
#pragma once

namespace cidx::detail {

extern const char *const kSchema;
extern const char *const kQualNameBackfill;

} // namespace cidx::detail

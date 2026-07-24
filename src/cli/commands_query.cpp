// The unified query command: parse CXQ, execute the shared QueryPlan, and
// emit the same stable JSON surface used by library callers.
#include "cli/commands_detail.hpp"

#include "query/cxq.hpp"
#include "query/exec.hpp"

namespace cidx::cli {

int cmd_query(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path, Storage::OpenMode::read_only);
  const query::Plan plan = query::parse_cxq(args.query_text);
  query::SqliteQueryReadAdapter read(db);
  query::Executor executor(read);

  const json_out::Value output = args.query_explain
                                     ? executor.explain(plan)
                                     : executor.run(plan).to_json();
  *ctx.out << json_out::dumps_indent2(output) << "\n";
  return 0;
}

} // namespace cidx::cli

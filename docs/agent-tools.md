# CIDX agent tools

The local agent surface is a versioned, newline-delimited JSON protocol named
`cidx.agent/v1`. It exposes exactly two tools: `query` and `explain`. Both
consume the typed application `QueryRequest` and the same CXQ parser and
QueryPlan executor used by `cidx query`.

## Request

Each line is one JSON object:

```json
{
  "version": 1,
  "tool": "query",
  "cxq": "codebase() | nodes() | select(name, file, line)",
  "budget": {"max_results": 1000}
}
```

`tool` is either `query` or `explain`; `cxq` is the textual CXQ accepted by the
CLI; and `budget.max_results` is an integer from 1 through 10,000. The version
is required. Unknown versions fail with the stable `E_PROTOCOL_VERSION`
diagnostic and are never downgraded.

The host binds the request to an existing read-only `QueryReadPort`. A request
cannot select a writable database handle or an indexing operation. The typed
request does not include CLI argument state or `ParsedArgs`.

## Response

Each request produces one JSON object:

```json
{
  "protocol": "cidx.agent/v1",
  "version": 1,
  "tool": "query",
  "response": {
    "protocol": "cidx.result/v1",
    "operation": "query",
    "status": "complete",
    "result": {"shape": "rows", "rows": []},
    "identity": {"freshness": "current"},
    "completeness": {"truncated": false, "budget": null}
  },
  "truncated": false,
  "budget": {
    "max_results": 1000,
    "exhausted": false,
    "exhausted_at": null
  }
}
```

`query.response.result` is byte-identical to the CLI's canonical query JSON.
`explain.response.result` is byte-identical to the CLI `--explain` JSON and
contains the normalized plan, execution shape, fixed execution budgets, and
input-relation completeness. `response.completeness.budget` and
`budget.exhausted_at` identify the request budget when output was truncated;
truncation is never represented as a complete result.

Every result carries the S-048 index identity and freshness. Stale indexes and
unverifiable, non-truncated results are reported as `unknown` with an explicit
diagnostic; a deliberately truncated result remains `partial` under the result
protocol while retaining its explicit `unverifiable` identity. Query rows can
request resolvable `file` and `line` fields, and path witnesses carry their
source-site evidence.

The public catalog is intentionally only `{query, explain}`. Noun-specific
tools are not aliases of this protocol and must not be added.

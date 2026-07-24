# CIDX result protocol v1

All new machine-readable results use `cidx.result/v1`. The envelope owns
meaning; renderers may choose a table or prose layout but must not upgrade a
status or infer completeness from a message.

The contract authority is `spec/contracts/result-protocol.json`. C++ and Python
domains, schemas, goldens, and acceptance vectors are generated from it; run
`uv run --project python python scripts/generate_result_protocol.py --check` to
verify that checked-in outputs are current.

## Universal fields

`operation`, `status`, `result`, `identity`, `producer`, `completeness`,
`diagnostics`, and `evidence` are universal. `result` is the typed,
operation-specific payload. `artifacts`, `replay`, and `resources` carry
optional references and execution metadata. Keys are emitted in the schema
order, arrays preserve producer order, and absent optional fields are omitted;
present nullable identity/resource fields are emitted as `null`.

`identity` always names the workspace, index, and fact sets. `freshness` is
independent from `status`: a stale index produces `status: unknown` and a
`stale_input` diagnostic. `completeness.truncated` is independent from status;
a truncated result is `partial`, never `complete`.

The status classes are `complete`, `partial`, `unknown`, `refuted`,
`conditional`, and `error`. `truncated`, `stale`, `timeout`, backend failure,
and refutation are represented by separate completeness or diagnostic codes,
not by renderer-specific prose.

## Exit reduction

The shared mapping is deterministic: success `0`, usage `2`, invalid or stale
input `3`, policy/refutation failure `4`, unknown or inconclusive `5`, and
infrastructure failure `6`. Adapters consume the envelope's `exit_class`; they
do not classify the payload a second time.

Long-running work emits one JSON object per line using `cidx.event/v1` on the
event stream. Progress and warnings are never written into the final JSON
payload. Human output includes the status, first actionable reason, and next
action for every non-complete result.

Evidence is a bounded tree (maximum depth four and 256 nodes). Result payloads,
arrays, properties, diagnostics, and replay arguments are bounded by the same
contract. Text, source
snippets, compiler arguments, environment-derived values, solver logs, and
extension output pass through the shared redaction and size-limit policy before
serialization. Human output is capped at 4096 UTF-8 bytes, with truncation only
at code-point boundaries.

Workspace identity is stable and derived from repository/component ownership
metadata using the same sorted owner list and NUL separator in both adapters
(`workspace:<sha1>`); adapters must not substitute `unknown`. When
freshness cannot be established, the result remains `unknown` rather than
claiming completeness.

Result numbers are signed 64-bit integers only; floating-point values,
non-finite numbers, and integers outside that range are rejected. Diagnostic
codes also constrain the envelope: `stale_input` requires an unknown result
over stale input, backend/timeout diagnostics require `error`, and
`policy_refuted` requires `refuted`. These cross-field rules are present in
both serializers and the generated JSON Schema.

The C++ adapter is `cidx::protocol::ResultEnvelope`; Python uses
`indexer.result_protocol.ResultEnvelope`. QueryPlan exposes
`Result.to_envelope_dict()` / `Result::to_envelope()` while retaining its
legacy shape during the compatibility migration.

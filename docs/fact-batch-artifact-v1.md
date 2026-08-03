# FactBatch artifact wire contract v1

`FactBatch` artifacts are the immutable transfer and cache representation of a
canonical C++ extraction batch. The authoritative API is
`src/ast/fact_batch_artifact.hpp`.

## Framing and digest

- Bytes are deterministic and little-endian.
- The payload starts with the eight-byte magic `CIDXFB01` and wire version `1`.
- Strings and collections use an unsigned 64-bit length followed by their
  bytes or elements. Optional values use one byte (`0` or `1`) before the
  value. Enums use their frozen unsigned byte value.
- The payload is followed by `FBDIG001` and the 71-byte lowercase
  `sha256:<64 hex>` digest of every preceding payload byte. The footer is not
  included in its own digest.

The field sequence is fixed: artifact identity metadata, all `FactRecords`
families in `FactRecords` declaration order, file partitions and membership
indices, then symbol, relation, type, definition, and file-handle dictionaries.
The v1 decoder rejects trailing or omitted payload fields; changing the order,
encoding, or record inventory requires a new wire version.

## Identity and compatibility

Every artifact records the producer and producer version from its canonical
batch plus explicit pass, extractor, storage-schema, and catalog versions. The
catalog identity contains both version and hash. Completeness, trust,
truncation, and sorted unique dependency identities are part of the digested
payload.

Readers fail closed by default:

- wire, pass, extractor, schema, catalog, or required trust mismatch, plus a
  producer identity/version mismatch when the reader requires one:
  `incompatible`;
- dependency identity mismatch: `stale`;
- non-complete metadata: `partial`;
- a truncation declaration or physically incomplete field: `truncated`;
- malformed framing, invalid enum/boolean, duplicate keys, noncanonical record
  or partition order, invalid partition indices, trailing fields, or digest
  mismatch: `corrupt`;
- missing files, configured byte/string/collection limits, and I/O failures
  retain their distinct diagnostic codes.

## Bounded transfer and replay

Encoding accepts only a batch whose complete record, partition, and apply-order
inventory satisfies the `canonical_batch()` invariants. It writes fields
incrementally to an in-memory buffer until the configured threshold, then
spills through the private descriptor returned by `mkstemp`. The encoder byte
threshold may be zero to deliberately disable buffering and write fields
directly to the spill file. Spill paths are unlinked immediately after secure
creation; the retained descriptor alone owns their lifetime. The encoder byte
limit defaults to the decoder byte limit, so it cannot emit an artifact that
the default reader rejects for size. The immutable, copyable handle streams or
materializes exactly its captured byte extent; file-backed handles retain one
descriptor so verification and decode observe the same file generation. Owned
spill descriptors close when the last handle is destroyed.

Decoding validates the byte limit and digest without copying the payload, then
checks declared lengths against both configured limits and the remaining
payload before allocation. It rejects payloads that do not satisfy canonical
record and partition ordering before marking the decoded batch canonical.
Compatibility failures expose diagnostics but not partially trusted metadata;
`info` is published only after the entire artifact validates.
Re-encoding a decoded v1 batch with the same metadata must reproduce
byte-identical output. The exhaustive fixture, authenticated negative corpus,
bounded-memory benchmark, and frozen SHA-256 vector live in
`tests/fact_batch_artifact_test.cpp`.

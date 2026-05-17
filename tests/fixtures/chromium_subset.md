# Chromium subset acquisition checklist

Story: S29-m5-chromium-gate
AC covered: AC-M5-10, AC-M5-11

This file documents how to obtain the Chromium `base/` + `net/` subtrees used
as the large-scale fixture for `tests/integration/m5_exit_gate.rs` (the
`m5_chromium_subset_gate` test).  The source trees are NOT committed to this
repository (the full Chromium checkout is > 30 GiB).

## Requirements

- A machine with at least 20 GiB of free disk space.
- `depot_tools` installed and on `PATH` (for `fetch chromium` / `gclient`).
- `gn` and `ninja` available (for generating `compile_commands.json`).
- Clang 18 on the machine (or a sysroot that provides it).

## Steps

### 1. Shallow fetch of Chromium

```bash
mkdir /data/chromium && cd /data/chromium
fetch --nohooks chromium
cd src
git fetch --depth 1
```

### 2. Generate compile_commands.json

```bash
cd /data/chromium/src
gn gen out/Default --args='is_debug=false is_component_build=false'
# Export compile commands for just the base/ and net/ components:
ninja -C out/Default -t compdb cc cxx > compile_commands.json
```

Only entries for files under `base/` and `net/` are needed.  Filter with:

```bash
python3 -c "
import json, sys
db = json.load(open('compile_commands.json'))
filtered = [e for e in db if '/base/' in e['file'] or '/net/' in e['file']]
json.dump(filtered, open('compile_commands_subset.json', 'w'), indent=2)
print(f'Kept {len(filtered)} of {len(db)} entries')
"
```

Place the filtered `compile_commands_subset.json` alongside the Chromium
checkout root and point `CXG_M5_CHROMIUM_PATH` at the `src/` directory.

### 3. Run the gate

```bash
CXG_M5_CHROMIUM_PATH=/data/chromium/src \
CXG_M5_CHROMIUM_CC=/data/chromium/src/compile_commands_subset.json \
    cargo nextest run -p cpp_indexer --test m5_exit_gate -- --ignored
```

## Expected output (passing)

```
m5_chromium_subset_gate: TUs processed: N  nodes: M  macro_nodes: K  expands_to_edges: J
m5_chromium_subset_gate: [AC-M5-10] exit=0  no_segfault=true  PASS
m5_chromium_subset_gate: [AC-M5-11] macro_nodes>=1  expands_to_edges>=1  PASS
```

## Notes

- The test uses `--skip-system-headers` so only project-local macro definitions
  are counted (system macros in `<stddef.h>` etc. are excluded).
- Chromium's `base/` directory defines several hundred project-local macros
  (`DISALLOW_COPY_AND_ASSIGN`, `DCHECK`, `LOG`, etc.).  A single TU is
  sufficient to produce ≥ 1 MACRO node and ≥ 1 EXPANDS_TO edge.
- The gate uses `MockSink` (no live DB required) — only Parquet output is
  inspected to satisfy AC-M5-11.
- `CXG_M5_CHROMIUM_CC` is optional; if unset, the pipeline auto-detects
  `compile_commands.json` by walking upward from `CXG_M5_CHROMIUM_PATH`.

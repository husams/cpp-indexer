# TLA+ toolchain contract

The repository uses the TLA+ command-line tools, not the graphical Toolbox.
The Toolbox is intentionally not part of the build or CI dependency surface.

| Dependency | Pin | Verification |
| --- | --- | --- |
| TLA+ tools | release `1.8.0`, `tla2tools.jar` | SHA-256 `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3` |
| Java runtime | major version `17` | `spec/tla/tools/check.sh` rejects any other major version |
| TLC workers | `1` | avoids small-model traversal/diameter nondeterminism |
| TLC fingerprint polynomial | `0` | fixed by the checker |
| TLC seed | `1` | fixes finite-state traversal and counterexample ordering |
| TLAPS (`tlapm`) | release `202210041448`, `tlaps-1.5.0-x86_64-linux-gnu-inst.bin` | SHA-256 `ebb7a3f271bdb564f74cb0a2767ef7b9ff7045621a9be7c50d363a03c2e6f08a`; `spec/tla/tools/check-proofs.sh` rejects any other reported `tlapm --version` |

TLAPS is a separate, native OCaml (Zenon) + Isabelle/ML toolchain -- it does
not use the Java/tla2tools pin above and needs a C toolchain (`cc`, `make`) to
compile its bundled Isabelle theories on first install into a cached prefix.
No Isabelle backend call is required for the proofs currently checked in;
Zenon alone discharges every obligation (`spec/tla/tools/check-proofs.sh`
still verifies the full toolchain installs so a future proof that does need
Isabelle is not silently unsupported).

The jar is fetched from the official release URL:

```text
https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
```

The exact checked command is:

```bash
spec/tla/tools/check.sh
```

The mutation regression is:

```bash
spec/tla/tools/check-regression.sh
```

The TLAPS proof gate is:

```bash
spec/tla/tools/check-proofs.sh
```

For an offline or pre-provisioned environment, point `TLA_TOOLS_JAR` at a
local copy of the same jar. The checker still verifies its SHA-256. `JAVA_BIN`
may select a Java 17 executable when `JAVA_HOME` is not suitable.
`TLA_PROOFS_INSTALLER` and `TLA_PROOFS_PREFIX` do the same for the TLAPS
installer and its (potentially cached, since Isabelle compilation is the
expensive step) install prefix.

The checker first invokes SANY for each model. Only after all syntax checks
pass does it invoke TLC. It uses temporary metadata and a disposable flattened
module directory, so no model-check output is written into the repository.

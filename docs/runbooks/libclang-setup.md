# Developer Guide: LIBCLANG_PATH Setup for libclang 18

**Audience:** Developers building cpp-indexer from source  
**Last updated:** 2026-05-17

---

## Overview

`cpp-indexer` links against libclang 18 at build time via the `clang` crate
(which uses `clang-sys` under the hood). The build system searches for
`libclang.so` (Linux) or `libclang.dylib` (macOS) on the dynamic linker path.
If the library is not found, the build fails with:

```
error: Unable to find libclang: "couldn't find any valid shared libraries matching: ...
```

Set `LIBCLANG_PATH` to the directory containing `libclang.so` /
`libclang.dylib` to resolve this.

---

## Linux (Debian / Ubuntu)

### Install libclang 18

```bash
# Add LLVM APT repository
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc

# For Ubuntu 22.04 (jammy):
echo "deb https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" \
  | sudo tee /etc/apt/sources.list.d/llvm.list

sudo apt update
sudo apt install -y libclang-18-dev clang-18
```

For other distributions, check https://apt.llvm.org for the matching
repository line.

### Locate the library

```bash
find /usr -name "libclang*.so*" 2>/dev/null
# Typical output: /usr/lib/llvm-18/lib/libclang.so.18.1
```

### Set LIBCLANG_PATH

```bash
export LIBCLANG_PATH=/usr/lib/llvm-18/lib
```

Add to `~/.profile` or `~/.bashrc` for persistence.

### Verify the build

```bash
LIBCLANG_PATH=/usr/lib/llvm-18/lib cargo build
```

---

## macOS

### Install libclang 18 via Homebrew

```bash
brew install llvm@18
```

Homebrew installs LLVM into a keg-only path (not on `PATH` by default):

```bash
brew --prefix llvm@18
# Typical output: /opt/homebrew/opt/llvm@18    (Apple Silicon)
#                 /usr/local/opt/llvm@18        (Intel)
```

### Locate the library

```bash
ls "$(brew --prefix llvm@18)/lib/libclang.dylib"
```

### Set LIBCLANG_PATH

```bash
export LIBCLANG_PATH="$(brew --prefix llvm@18)/lib"
```

Add to `~/.zshrc` or `~/.bash_profile` for persistence.

### Verify the build

```bash
LIBCLANG_PATH="$(brew --prefix llvm@18)/lib" cargo build
```

---

## Docker / CI

The `Dockerfile` ships `libclang-18-dev` and `clang-18` from
`debian:bookworm-slim`. No manual `LIBCLANG_PATH` is needed inside the
container because the library is on the default linker path.

For CI jobs running outside Docker, set `LIBCLANG_PATH` as an environment
variable in the CI configuration before running `cargo build`.

---

## Troubleshooting

### Wrong version linked

```bash
clang --version    # confirm: "clang version 18.x"
```

If `clang` points to a different version, either install clang-18 and use
`update-alternatives`, or set `LIBCLANG_PATH` explicitly to the clang-18 lib
directory.

### Multiple LLVM versions installed

Set `LIBCLANG_PATH` to the explicit clang-18 path rather than relying on the
default search. The `clang-sys` crate's version probe can select the wrong
version when multiple LLVM installations exist.

### Runtime linker cannot find the library

After building, if the binary fails to load:

```
error while loading shared libraries: libclang.so.18.1: cannot open shared object file
```

Either:

- Add the library directory to `LD_LIBRARY_PATH` at runtime (Linux), or
- Copy/symlink `libclang.so.18.1` to `/usr/local/lib` and run `ldconfig`.

The Docker image handles this automatically.

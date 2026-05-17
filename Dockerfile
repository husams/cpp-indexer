# ============================================================
# Stage 1: builder
#   - rust:1-bookworm provides rustup-managed stable toolchain
#   - libclang-18-dev + clang-18 from llvm.org APT repository
# ============================================================
FROM rust:1-bookworm AS builder

# Install libclang 18 from the official LLVM APT repository.
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        wget gnupg ca-certificates && \
    wget -qO /etc/apt/trusted.gpg.d/llvm.asc \
        https://apt.llvm.org/llvm-snapshot.gpg.key && \
    echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-18 main" \
        > /etc/apt/sources.list.d/llvm-18.list && \
    apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        libclang-18-dev clang-18 && \
    rm -rf /var/lib/apt/lists/*

ENV LIBCLANG_PATH=/usr/lib/llvm-18/lib
# Serialise rustc invocations to limit peak memory usage in resource-constrained
# environments (e.g. Docker Desktop with ≤2 GiB). CI runners with ≥4 GiB will
# override via CARGO_BUILD_JOBS at build time if desired.
ENV CARGO_BUILD_JOBS=1
ENV CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1

WORKDIR /build

# Copy the full source tree and build.
COPY . .

RUN cargo build --release \
    --bin cxg-index \
    --bin cxg-resolve-cross-repo \
    --bin cxg-daemon

# ============================================================
# Stage 2: runtime
#   - debian:bookworm-slim with only the runtime LLVM libraries
# ============================================================
FROM debian:bookworm-slim AS runtime

# Install LLVM 18 runtime libraries (not -dev) from apt.llvm.org.
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        wget gnupg ca-certificates && \
    wget -qO /etc/apt/trusted.gpg.d/llvm.asc \
        https://apt.llvm.org/llvm-snapshot.gpg.key && \
    echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-18 main" \
        > /etc/apt/sources.list.d/llvm-18.list && \
    apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        libclang-cpp18 libllvm18 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/target/release/cxg-index              /usr/local/bin/cxg-index
COPY --from=builder /build/target/release/cxg-resolve-cross-repo /usr/local/bin/cxg-resolve-cross-repo
COPY --from=builder /build/target/release/cxg-daemon             /usr/local/bin/cxg-daemon

# Default command is the daemon.
# ENTRYPOINT is intentionally omitted so the caller can override:
#   docker run <image> cxg-index --version
#   docker run <image> cxg-resolve-cross-repo --help
# Without a hard ENTRYPOINT, those resolve directly to the named binary.
CMD ["/usr/local/bin/cxg-daemon"]

# Convenience wrapper around the CMake build (the canonical build system).
# `make`               -> default dynamic build in build/
# `make static`        -> build in build-static/ (-DCIDX_STATIC=ON): SQLite3 is
#                         static; the Clang C++ API (clang-cpp + libLLVM) stays
#                         dynamic on every platform (a static libstdc++ would
#                         corrupt the ABI across the LLVM boundary).
# `make test` / `make test-static` -> run the ctest suite for that build.
#
# These targets only shell out to cmake; the build logic lives in CMakeLists.txt
# so there is a single source of truth. LLVM/Clang are discovered automatically
# (llvm-config --cmakedir + Homebrew hints); override with
# `make CMAKE_ARGS="-DLLVM_DIR=... -DClang_DIR=..."` if discovery fails.

BUILD_DIR        ?= build
STATIC_BUILD_DIR ?= build-static
JOBS             ?= $(shell (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4))
CMAKE_ARGS       ?=

.PHONY: all build static test test-static clean help

all: build

build:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) -j $(JOBS)

static:
	cmake -S . -B $(STATIC_BUILD_DIR) -DCIDX_STATIC=ON $(CMAKE_ARGS)
	cmake --build $(STATIC_BUILD_DIR) -j $(JOBS)

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

test-static: static
	cd $(STATIC_BUILD_DIR) && ctest --output-on-failure

clean:
	rm -rf $(BUILD_DIR) $(STATIC_BUILD_DIR)

help:
	@echo "targets: build (default), static, test, test-static, clean"
	@echo "vars:    BUILD_DIR, STATIC_BUILD_DIR, JOBS, CMAKE_ARGS"

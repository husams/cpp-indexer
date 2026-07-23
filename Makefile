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

# Absolute repo path, so every target works no matter the caller's cwd.
REPO_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

.PHONY: all build static test test-static test-e2e tidy contracts-check clean help

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

# BDD end-to-end suite: indexes tests/e2e/fixtures through the cidx CLI and
# verifies the result through the Python graph-query API. Needs the binary.
test-e2e: build
	CIDX_BIN=$(REPO_DIR)/$(BUILD_DIR)/cidx \
	  uv run --project $(REPO_DIR)/python pytest $(REPO_DIR)/tests/e2e $(PYTEST_ARGS)

# Analyze project-owned C++ with the checked-in .clang-tidy policies. CMake's
# compile database supplies the real C++23 flags and include paths.
tidy:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) --target cidx-clang-tidy -j $(JOBS)

contracts-check:
	uv run --project $(REPO_DIR)/python python $(REPO_DIR)/scripts/check_release_contract.py

clean:
	rm -rf $(BUILD_DIR) $(STATIC_BUILD_DIR)

help:
	@echo "targets: build (default), static, test, test-static, test-e2e, tidy, contracts-check, clean"
	@echo "vars:    BUILD_DIR, STATIC_BUILD_DIR, JOBS, CMAKE_ARGS, PYTEST_ARGS"

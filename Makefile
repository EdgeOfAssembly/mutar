# µtar (mutar) — convenience targets for test + formal verification
# Primary build is CMake; this Makefile wraps ctest and CBMC (Phase G / G19).
#
# Usage:
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug ...
#   cmake --build build -j$(nproc)
#   make test
#   make verify

BUILD   ?= build
# Prefer PATH, else ~/.local/bin/cbmc (this host's CBMC 6.10 install).
CBMC    ?= $(shell c=$$(command -v cbmc 2>/dev/null); \
	if [ -n "$$c" ]; then echo "$$c"; \
	elif [ -x "$(HOME)/.local/bin/cbmc" ]; then echo "$(HOME)/.local/bin/cbmc"; fi)
CC      ?= gcc
CFLAGS  ?= -std=c23 -Wall -Wextra -O1 -Iformal

# Profile build: -O3 + -pg + -fno-inline (gprof). See gnu-make skill.
PROFILE_DIR ?= build-profile
CXXFLAGS_PROFILE := -std=c++23 -O3 -DNDEBUG -g -pg -fno-inline \
	-march=x86-64 -mtune=generic -fno-omit-frame-pointer
LDFLAGS_PROFILE  := -pg

.PHONY: test tests verify formal-agreement formal-cbmc formal-clean profile release

# ── Unit / integration tests (CTest) ──────────────────────────────────────────
test tests:
	@test -d $(BUILD) || { echo "error: $(BUILD)/ missing — run cmake -B $(BUILD) first"; exit 1; }
	ctest --test-dir $(BUILD) --output-on-failure

# ── Formal agreement fixtures (always, no CBMC required) ─────────────────────
formal/path_agreement_test: formal/path_agreement_test.c formal/path_sanitize.c formal/path_sanitize.h
	$(CC) $(CFLAGS) -o $@ formal/path_agreement_test.c formal/path_sanitize.c

formal-agreement: formal/path_agreement_test
	./formal/path_agreement_test

# ── CBMC (optional if installed) ──────────────────────────────────────────────
# Unwind bound covers component walk for short nondet paths (N=12 in harness).
formal-cbmc:
	@if [ -z "$(CBMC)" ]; then \
	  echo "formal: CBMC not run (cbmc not found in PATH or ~/.local/bin)"; \
	  exit 0; \
	fi
	@echo "formal: running CBMC with $(CBMC)"
	# Nondet harness: bounds-check + unwind 6 (pointer-check optional; heavier)
	$(CBMC) formal/path_harness.c formal/path_sanitize.c \
	  --function harness \
	  --bounds-check \
	  --unwind 6 \
	  --stop-on-fail \
	  --verbosity 4
	# Concrete fixtures: full bounds + pointer checks
	$(CBMC) formal/path_harness.c formal/path_sanitize.c \
	  --function harness_fixtures \
	  --bounds-check \
	  --pointer-check \
	  --unwind 16 \
	  --unwinding-assertions \
	  --stop-on-fail \
	  --verbosity 4
	@echo "formal: CBMC properties VERIFIED"

# ── verify = tests first, then formal (rule 09) ───────────────────────────────
verify: test formal-agreement
	@if [ -n "$(CBMC)" ]; then \
	  $(MAKE) formal-cbmc CBMC="$(CBMC)"; \
	else \
	  echo "formal: CBMC not run (cbmc not found); agreement fixtures passed"; \
	fi
	@echo "formal: verify complete"

formal-clean:
	rm -f formal/path_agreement_test

# ── Profile (gprof) ───────────────────────────────────────────────────────────
# Usage: make -s -j$(nproc) profile
# Then run build-profile/mutar … ; gprof build-profile/mutar gmon.out
profile:
	cmake -S . -B $(PROFILE_DIR) \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_CXX_FLAGS="$(CXXFLAGS_PROFILE)" \
	  -DCMAKE_EXE_LINKER_FLAGS="$(LDFLAGS_PROFILE)" \
	  -DCMAKE_C_FLAGS="$(CXXFLAGS_PROFILE)"
	cmake --build $(PROFILE_DIR) -j$$(nproc 2>/dev/null || echo 1)
	@echo "Profile binary: $(PROFILE_DIR)/mutar  (run it, then: gprof $(PROFILE_DIR)/mutar gmon.out)"

# ── Release (no -pg) ──────────────────────────────────────────────────────────
release:
	cmake -S . -B build-release \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_CXX_FLAGS="-std=c++23 -O3 -DNDEBUG -march=x86-64 -mtune=generic" \
	  -DCMAKE_EXE_LINKER_FLAGS=""
	cmake --build build-release -j$$(nproc 2>/dev/null || echo 1)
	@echo "Release binary: build-release/mutar"

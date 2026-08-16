# AGENTS.md — µtar (mutar) Agent Instructions

This file is read by coding agents before starting work in the mutar project.
Active backlog: `GOAL_NEXT.md`. Archived rename campaign: `GOAL.md`.

---

## Quick-Start Checklist for mutar Work

Before writing any code:

- [ ] Read `AGENTS.md` (this file) and `GOAL.md` / `GOAL_NEXT.md` as applicable
- [ ] **Read `tar.1` in full** — this is the authoritative spec
- [ ] Install required tools (see below)

---

## Install Required Tools Before Any Testing

```bash
sudo apt-get update && sudo apt-get install -y \
  gzip bzip2 xz-utils zstd lzma lzip lzop \
  gdb valgrind \
  cmake build-essential
# Note: sanitizer runtimes (libasan, libubsan) ship with GCC/Clang; no separate install needed.
```

---

## Build with Sanitizers (Mandatory for Debug/Test)

```bash
cd /path/to/mutar
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-std=c++23 -ggdb3 -O0 -gdwarf-5 \
                     -fvar-tracking-assignments -fno-omit-frame-pointer \
                     -fstack-protector-all \
                     -fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

---

## The Prime Directive for mutar

> **Read `tar.1` before claiming any option is implemented.**

The man page is the ground truth. If `src/mutar.cpp` says it handles `--pax-option` but the code
is `break; // no-op`, then `--pax-option` is **not implemented**, regardless of what any
previous PR claimed.

---

## Honest Status Reporting

Every PR for mutar must include a status table like this:

| Option | Status | Notes |
|--------|--------|-------|
| `-c`, `--create` | ✅ Implemented | Full parity with tar.1 |
| `-S`, `--sparse` | ⚠️ Partial | Sparse write/extract works; `--hole-detection` wired; `--sparse-version` string-only (always emit 1.0) |
| `--pax-option` | ❌ No-op | Parsed and discarded; no Config field |
| `--verify` | ✅ Implemented | Post-create re-read verification |

Status key:
- ✅ **Implemented** — observable behavior matches tar.1; tested
- ⚠️ **Partial** — some behavior but not full tar.1 parity
- ❌ **No-op / Stub** — parsed but does nothing

---

## Testing Requirements for mutar

### Mandatory Coverage

1. **Format round-trip tests** — for every claimed format (`v7`, `oldgnu`, `gnu`, `ustar`,
   `pax`): create → extract → compare (file count, sizes, MD5 checksums).

2. **Compression round-trip tests** — for every claimed compressor (`-z`/gzip, `-j`/bzip2,
   `-J`/xz, `--zstd`): create → extract → compare.

3. **Sparse file tests** — create a sparse file, archive with `-S`, extract, verify the
   extracted file is still sparse (or bit-identical).

4. **Boundary tests** — `tests/boundary_tests.cpp` and the `mutar_boundary_tests` CTest
   target are the canonical model.

5. **Sanitizer-clean** — all tests must pass with `-fsanitize=address,undefined
   -fno-sanitize-recover=all` enabled. Any sanitizer error is a blocker.

### Test Output as Proof

PR descriptions must include copied test output showing:
- CTest summary (`N tests passed, 0 failed`)
- At minimum a sample of format/compression round-trip pass lines
- Sanitizer-clean confirmation (`no errors detected`)

---

## Help Text Requirements

Every PR that touches option parsing must update `print_usage()` in `src/mutar.cpp`:

- All implemented options must appear in help text.
- No-op / stub options must either be absent from help or marked `(not yet implemented)`.
- Options must be grouped like `tar.1`: Main operations, Format/Compression, File selection,
  Extraction behavior, Ownership/Permissions, Informational.

---

## Documentation Update Checklist

For any PR changing mutar behavior:

- [ ] `src/mutar.cpp` — update `print_usage()` if options added/changed
- [ ] `README.md` — update feature status table
- [ ] `ARCHITECTURE.md` — update if implementation approach changed
- [ ] `COMPATIBILITY_PROGRESS.md` — add proof entry: what was done, how it was tested, what remains

---

## Known Gaps (do not claim full tar.1 parity)

See `COMPATIBILITY_PROGRESS.md` for the full option audit. Short list matching code truth:

**True no-ops / broken parse:**
- `--pax-option` — parsed, discarded; no Config field
- `--volno-file` — field exists but never assigned from CLI
- `--check-device` / `--no-check-device` — pure discard; no Config field
- `--quoting-style` — stored, never used for list/verbose output
- `--restrict` — stored (`restrict_opt`), not enforced
- SELinux (`--selinux` / `--no-selinux`) — policy-unsupported no-op + warning

**Partial (do not claim complete):**
- Multi-volume (`-M`): naming/prompts exist; rotation dead without numeric `tape_length` (CLI only stores string via `-L`)
- `--info-script` / `--new-volume-script` — stored, never executed
- `--xattrs` / `--acls` (and include/exclude) — flags only; no store/restore
- `--backup` / `--suffix` — simple suffix rename works; numbered/existing CONTROL not implemented
- `-s` / `--preserve-order` — accepted with not-implemented warning
- `--sparse-version` — string stored; write hardcodes 1.0

**Implemented (do not list as no-ops):** `--verify`, `--hole-detection`, `--owner-map` / `--group-map`, `--exclude-vcs-ignores`, `--index-file`, `--checkpoint-action`, `--interactive`, `--full-time`, `--warning`, wildcards/anchoring, `--overwrite-dir` / `--no-overwrite-dir`, and related PR #170/#172 features.

---

## Security Rules for mutar

- Temporary work directories: use `mkdtemp()`, not `/tmp/mutar_fixed_name`.
- Archive extraction paths: validate that extracted paths do not escape the destination
  directory (path traversal / zip-slip vulnerability).
- Size inputs: validate `entry_size` and block counts before allocation.
- Symlink handling: follow `--dereference` correctly; do not blindly follow symlinks during
  extraction in ways that could write outside the target directory.

---

*Read repo root `AGENTS.md` and `.github/copilot-instructions.md` before this file.*


## SELinux policy

SELinux (`--selinux` / `--no-selinux`) is **unsupported** by project policy (no test hardware).
Options are accepted as no-ops with a warning. Do not claim SELinux support.
Compatibility goal is **~99%** GNU tar 1.35, not 100%.

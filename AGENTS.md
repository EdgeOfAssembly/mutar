# AGENTS.md — µtar (mutar) Agent Instructions

This file is read by coding agents before starting work in the mutar project.
Active campaign: `GOAL_GNU_PARITY.md` (100% GNU tar CLI except SELinux).
Archived: `GOAL.md`, `GOAL_NEXT.md`.

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
| `-S`, `--sparse` | ✅ Implemented | Sparse write/extract; `--hole-detection`; `--sparse-version` 0.0/0.1/1.0 |
| `--pax-option` | ✅ Implemented | Full GNU set: delete=, exthdr.*, globexthdr.*, keyword=/:= |
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
- SELinux (`--selinux` / `--no-selinux`) — policy-unsupported no-op + warning

**Partial / limited (do not claim complete GNU parity):**
- Listed-incremental **write** uses `MUTAR_SNAPSHOT_V2` text (GNU format 2 **read** is supported best-effort)
- True compressed frame-level seek without materialize (see `TODO.md`)

**Implemented (do not list as no-ops):** multi-volume mid-file (`GNUTYPE_MULTIVOL` 'M') including **sparse**, compressed remote (materialize), `--mode` symbolic, `-N` ctime vs `--newer-mtime`, `-g` GNU snapshot read, rmt O/R/W/L/C + remote -r/-u, `-s`/`--preserve-order`, `--pax-option` (full GNU set), `--restrict`, `--backup` CONTROL, `--quoting-style`, `--check-device`/`--no-check-device`, `--xattrs` / `--acls`, `--verify`, `--hole-detection`, `--owner-map` / `--group-map`, `--exclude-vcs-ignores`, `--exclude-ignore` / `--exclude-ignore-recursive`, `-G` dumpdir, `-g` listed-incremental skip, `--sparse-version` 0.0/0.1/1.0, `--index-file`, `--checkpoint-action`, `--interactive`, `--full-time`, `--warning` (broad `mutar_warn` coverage), wildcards/anchoring, `--overwrite-dir` / `--no-overwrite-dir`, Phase 1–8 features.

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

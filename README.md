# µtar (`mutar`) — Modern C++23 GNU-tar-compatible archiver

**µtar** (binary: **`mutar`**) is a ground-up C++23 reimplementation of GNU tar.

It is **not** Jörg Schilling’s `star` (Schily tools).

**Compatibility goal: ~99% with GNU tar 1.35** for common formats
(v7, oldgnu, gnu, ustar, pax/posix) and the command-line interface in `tar(1)`.
**SELinux is not supported** (no test hardware). Optional **sidecar index** (`--write-index` / `--mutar-index`) and **`--seekable`** enable fast list and seek extract (uncompressed direct seek; compressed materialize-then-seek — full decompress once, not frame-level seek). Run `./mutar --help` for options.

Performance claims require published benchmarks; see `PROGRESS.md` Phase F.

> See `COMPATIBILITY_PROGRESS.md` for an option-by-option audit and
> `ARCHITECTURE.md` for design details. Archived campaign: `GOAL.md`.
> **Next work:** `GOAL_NEXT.md`.

## Status

**v0.1.0 freeze (2026-08-16).** Campaign `GOAL.md` phases 0–7 complete.

| Area | Status |
|------|--------|
| Identity | µtar / `mutar` / [EdgeOfAssembly/mutar](https://github.com/EdgeOfAssembly/mutar) |
| GNU tar interop | Formats + common CLI; **~99%** goal |
| SELinux | Unsupported |
| Sidecar index | `--write-index` / `--mutar-index` |
| Seek | Uncompressed direct `lseek`; compressed **materialize-then-seek** (`--seekable`) — not frame-level |

Proof log: `PROGRESS.md`. Option audit: `COMPATIBILITY_PROGRESS.md`.

---

## Quick Start

```bash
# Build (debug — full GDB symbols by default)
cd mutar && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Build (release — optimised)
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## Usage

```
mutar [OPTION...] [FILE...]
```

### Main Operations

| Flag | Long form | Description |
|------|-----------|-------------|
| `-c` | `--create` | Create a new archive |
| `-x` | `--extract`, `--get` | Extract files from an archive |
| `-t` | `--list` | List archive contents |
| `-r` | `--append` | Append files to archive |
| `-u` | `--update` | Append only files newer than archive copy |
| `-A` | `--catenate`, `--concatenate` | Append tar files to an archive |
| `-d` | `--diff`, `--compare` | Find differences vs filesystem |
|      | `--delete` | Delete members from archive |
|      | `--test-label` | Test/print volume label |

### Common Options

| Flag | Description |
|------|-------------|
| `-f ARCHIVE` | Archive file (or `-` for stdin/stdout) |
| `-v` | Verbose — list files processed |
| `-z` | Compress with gzip |
| `-j` | Compress with bzip2 |
| `-J` | Compress with xz |
| `--zstd` | Compress with zstd |
| `-C DIR` | Change to directory DIR |
| `-P` | Don't strip leading `/` from paths |
| `-p` | Preserve file permissions |
| `-k` | Don't overwrite existing files |
| `-S` | Handle sparse files |
| `--write-index` | Write sidecar member index (`ARCHIVE.mutaridx`) |
| `--mutar-index=FILE` | Explicit index path (create/list/extract) |
| `--seekable` | Seek-friendly xz/zstd blocks; implies index; compressed selective extract materializes once then seeks (not frame-level) |
| `-H FORMAT` | Archive format: `v7` `oldgnu` `gnu` `ustar` `pax` |
| `--strip-components=N` | Strip N leading components from paths on extract |
| `--exclude=PATTERN` | Exclude files matching PATTERN |
| `--owner=NAME` | Force owner for created entries |
| `--group=NAME` | Force group for created entries |
| `--numeric-owner` | Always use numeric UID/GID |
| `--posix` | Create POSIX (pax) format archive |
| `-O` | Extract to stdout |

### Examples

```bash
# Create a gzip-compressed archive
mutar -czf project.tar.gz src/

# Extract with verbose output
mutar -xvf project.tar.gz

# Extract to a specific directory
mutar -xf archive.tar -C /tmp/dest

# List contents of an xz-compressed archive
mutar -tJf tar-1.35.tar.xz

# Create POSIX/PAX format (supports filenames > 100 chars, nanosecond timestamps)
mutar --posix -cf archive.tar very-long-path/

# Extract only specific files
mutar -xf archive.tar file1.txt dir/file2.txt

# Create with exclusions
mutar -czf backup.tar.gz . --exclude='*.o' --exclude='.git'

# Strip leading path component on extract
mutar -xf archive.tar --strip-components=1

# Post-create read-back verification
mutar -czf backup.tar.gz src/ --verify

# Interactive extraction with confirmation prompts
mutar -xf archive.tar --interactive

# Show all available options
mutar --help
```

---

## Archive Format Compatibility

| Format | Read | Write | Notes |
|--------|------|-------|-------|
| **v7** | ✅ | ✅ | Original Unix V7, no uname/gname/devs |
| **oldgnu** | ✅ | ✅ | GNU tar ≤ 1.12; atime/ctime in header |
| **gnu** | ✅ | ✅ | Default write format; LongName/LongLink extensions |
| **ustar** | ✅ | ✅ | POSIX.1-1988; 100-char name + 155-char prefix |
| **pax/posix** | ✅ | ✅ | POSIX.1-2001; unlimited names, nanosecond mtimes |

All 7 archives in `src/` are read successfully:
`cbmfs`, `dosbox`, `iir1`, `sidplay-2.0.9`, `sidplay-libs`, `capstone`, `tar-1.35`.

---

## Compression Support

| Flag | Program | Extensions | CI available |
|------|---------|------------|-------------|
| `-z` | `gzip` | `.tar.gz`, `.tgz` | ✅ |
| `-j` | `bzip2` | `.tar.bz2`, `.tbz2` | ✅ |
| `-J` | `xz` | `.tar.xz`, `.txz` | ✅ |
| `--zstd` | `zstd` | `.tar.zst` | ✅ |
| `--lzma` | `lzma` | `.tar.lzma` | ✅ |
| `--lzip` | `lzip` | `.tar.lz` | ❌ (tests skip) |
| `--lzop` | `lzop` | `.tar.lzo` | ❌ (tests skip) |
| `--compress` | `compress` | `.tar.Z` | ❌ (tests skip) |
| `--use-compress-program=PROG` | custom | any | n/a |
| `-a` | auto-detect | from filename extension | n/a |

Magic-byte auto-detection works on read even without `-a`.

---

## Switch Compatibility Status

### Fully Implemented (PR #167 – PR #170)

- All main operations: `-c -x -t -r -u -A -d --delete --test-label`
- Compression: `-z -j -J --zstd --lzma --lzip --lzop --use-compress-program -a --no-auto-compress`
- Format selection: `-H v7/oldgnu/gnu/ustar/pax` `--posix` `--old-archive`
- File attributes: `--owner --group --mtime --mode --numeric-owner --same-owner --no-same-owner`
- Permissions: `-p --same-permissions --no-same-permissions --preserve-permissions`
- Overwrite control: `-k --skip-old-files --keep-newer-files --overwrite -U --recursive-unlink`
- Path handling: `-P --strip-components --transform/--xform --exclude --exclude-from -X`
- File selection: `--newer/-N --newer-mtime --exclude-vcs --exclude-backups`
- Blocking: `-b --record-size -i --ignore-zeros`
- Timestamps: `-m --touch --atime-preserve --utc --full-time` (nanosecond precision in `--list`)
- Extraction: `-O --to-stdout --to-command -C --one-top-level`
- Source management: `--remove-files --one-file-system`
- Hard links: detected on create (nlink>1 → LNKTYPE entries); restored on extract
- Sparse files: `--sparse/-S` — write with SEEK_DATA/SEEK_HOLE; extract preserves holes; `--hole-detection=seek/raw`
- Wildcard exclusion: `--anchored` / `--no-anchored`, `--ignore-case` / `--no-ignore-case`, `--wildcards-match-slash` / `--no-wildcards-match-slash`
- Informational: `-v -R --block-number --totals --show-omitted-dirs --show-transformed-names`
- Sorting: `--sort=name`
- Labels: `-V --test-label`
- Incremental: `--level=0` for level-0 snapshot archives
- Index output: `--index-file=FILE` during create/list/extract (verbose output routed exclusively to file)
- Checkpoints: `--checkpoint=N` `--checkpoint-action=dot/echo/ttyout` (default: print message)
- Exclusion: `--exclude-vcs-ignores` (reads `.gitignore`, `.hgignore`, `.cvsignore`, `.bzrignore`)
- Verification: `--verify` / `-W` — post-create read-back verification
- Interaction: `--interactive` / `--confirmation` — per-file confirmation prompts (reads from `/dev/tty`)
- Warnings: `--warning=KEYWORD` — `mutar_warn()` wired at key emission sites
- Help: `--help` prints all ~100 options; `--version` prints version string

### Accepted (no-op or partial — honest status)

| Option | Status | Notes |
|--------|--------|-------|
| `--pax-option` | ⚠️ Partial | `delete=KEYWORD` applied on write; other keywords accepted silently |
| `--volno-file` | ✅ | Atomic read/write of current volume number |
| `--check-device` / `--no-check-device` | ✅ | Config `check_device` (default on); snapshot V2 stores `st_dev`; re-archive when device changes |
| `--info-script` / `--new-volume-script` | ✅ | Exec'd at volume boundary; TAR_ARCHIVE/TAR_VOLUME; non-zero fails |
| `--restrict` | ✅ | Rejects `-P`/`--absolute-names`, `--to-command`, multi-volume (`-M`/`-L`/`-F`) |
| `--quoting-style` | ✅ | `literal`/`escape`/`c`/`c-maybe`/`shell`/`shell-always` for `-t` and verbose extract |
| `--xattrs` / `--acls` | ✅ | Store/restore via PAX `SCHILY.xattr.*` / `SCHILY.acl.*` when built with lib support; SELinux never stored |
| `--selinux` / `--no-selinux` | ❌ Unsupported | Policy: no-op + warning (no test hardware) |
| `-G -g --listed-incremental` | ⚠️ Partial | Snapshot V2 records files+dirs (mtime+dev); skip filter is regular-file mtime (+dev); dirs always dumped |
| `--multi-volume -M -L` | ⚠️ Partial | Between-member rotation + extract stream swap; mid-file split not supported |
| `--rsh-command --rmt-command` | ⚠️ Partial | rmt O/R/W/C bridge via rsh; lseek (`S`) / remote append not supported |
| `--backup --suffix` | ✅ | `none`/`off`, `simple`/`never`, `numbered`/`t`, `existing`/`nil` (+ `--suffix`) |
| `-s / --preserve-order` | ⚠️ Partial | Accepted (emits warning); not yet wired into traversal |
| `--sparse-version` | ⚠️ Partial | String stored; write path hardcodes GNU.sparse 1.0 |
| `--owner-map` / `--group-map` | ✅ | Loaded and applied at create time (PR #172) |
| `--overwrite-dir` / `--no-overwrite-dir` | ✅ | Wired in DIRTYPE extract (PR #172) |
| `--warning=KEYWORD` | ✅ | `mutar_warn()` wired at key emission sites (PR #172) |

### Known Gaps / Future Work

| Feature | Notes |
|---------|-------|
| SELinux | Unsupported by policy (`--selinux` no-op + warning); never stored under `--xattrs` |
| Incremental backups (`-G -g`) | Snapshot V2 has dirs+dev; skip still regular-file only (dirs always dumped) |
| Remote tape (`--rsh/rmt-command`) | Bridge works; lseek over rmt / remote append not implemented |
| Multi-volume mid-file split | Between-member works; single file > tape length errors (no GNUTYPE_MULTIVOL) |
| `--pax-option` keyword processing | `delete=KEYWORD` only; full GNU set not implemented |

---

## Bug Fixes

### PR #168

| Bug | Impact | Fix |
|-----|--------|-----|
| **AREGTYPE (`typeflag=0x00`) treated as directory** | **Critical** — all regular files from old GNU/V7 archives silently created as empty directories. | Split `case DIRTYPE:` from `case 0:`. `AREGTYPE` now correctly handled as regular file in `default:`. |
| **No-op options** | Moderate — `--newer`, `--mtime`, `--mode`, `--transform`, `--ignore-zeros`, `--totals`, `--block-number`, `--record-size`, `--remove-files`, `--atime-preserve`, `--one-file-system`, `--to-command`, `--one-top-level`, `--newer-mtime` parsed but never acted upon. | Fully implemented in PR #168. |
| **Hard link data duplication** | Moderate — `seen_inodes` map declared but never consulted. | Now detects nlink>1 and emits LNKTYPE entries; restored correctly on extract. |
| **Sparse files wrote dense data** | Moderate — `--sparse/-S` accepted but files always written densely. | Now uses SEEK_DATA/SEEK_HOLE; writes GNU 'S' headers; extract seeks to correct offsets. |

### PR #170

| Feature | Detail |
|---------|--------|
| `--verify` | Post-create re-read verification implemented in `op_create()` |
| `--interactive` | Per-file confirmation prompts during extract (reads from `/dev/tty`) |
| `--warning=KEYWORD` | Parsed into enable/disable sets; emission wired via `mutar_warn()` in PR #172 |
| `--exclude` anchoring | `--anchored/--ignore-case/--wildcards-match-slash` wired into `is_excluded()` |
| `--show-omitted-dirs` | Wired: excluded dirs during create and unmatched dirs during list |
| `--show-transformed-names` | Wired into create transform path |
| `--index-file` | Verbose output routed exclusively to file during create/list/extract |
| `--checkpoint-action` | `dot/echo/ttyout` actions; default (no action) prints `checkpoint N` message |
| `--full-time` | Nanosecond timestamps in `--list` output |
| `-s / --preserve-order` | Accepted; emits not-implemented warning (see partial table) |
| `--overwrite-dir` | Fully wired in DIRTYPE extract (PR #172) |
| `--exclude-vcs-ignores` | Reads `.gitignore`, `.hgignore`, `.cvsignore`, `.bzrignore` |
| `--hole-detection=seek/raw` | Wired into `detect_sparse_segments()`; implies `--sparse` |
| `--level=0` | Level-0 incremental archive creation |
| `--help` | Comprehensive output showing all ~100 options |

---

## Building

### Requirements

- GCC ≥ 13 (C++23: `std::expected`, `std::format`, `std::ranges`)
- CMake ≥ 3.20
- No external C++ library dependencies (pure POSIX + stdlib)

### Build Types

```bash
# Debug (default): -ggdb3 -O0 -gdwarf-5 -fvar-tracking-assignments
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Release: -O2 -DNDEBUG
cmake .. -DCMAKE_BUILD_TYPE=Release
```

Debug builds follow the repository's
[`ubuntu-gdb-max-debug-best-practices.txt`](../ubuntu-gdb-max-debug-best-practices.txt)
exactly — DWARF 5, full variable tracking, no frame-pointer omission.

### CTest Integration

```bash
cd build && ctest --output-on-failure   # 2/2 CTest suites
```

---

## Testing

Four test suites are provided:

```bash
# Core functionality — 41 tests, 0 failures
bash tests/run_tests.sh build/mutar

# Format + compression round-trips — 59 pass, 27 skip (missing lzip/lzop/compress)
bash tests/test_formats_compression.sh build/mutar

# Sparse file handling — 20 pass, 3 skip
bash tests/test_sparse.sh build/mutar

# New PR #170 options — 20 pass, 0 failures
bash tests/test_new_options.sh build/mutar
```

### `tests/run_tests.sh` — 41 tests

| ID | Test |
|----|------|
| T01 | Create + list |
| T02 | Create + extract + content verify |
| T03 | Verbose listing format |
| T04 | Symlink preservation |
| T05–T07 | gzip / bzip2 / xz round-trip |
| T08 | Interop: mutar reads system-tar archive |
| T09 | Interop: system tar reads mutar archive |
| T10 | Long filename (GNU LongName extension) |
| T11 | USTAR format + magic byte check |
| T12 | PAX format (extended headers) |
| T13 | Selective member extraction |
| T14 | `--strip-components` |
| T15 | `--exclude` pattern (basename match) |
| T16 | Extract to stdout (`-O`) |
| T17 | Read `tar-1.35.tar.xz` from repo |
| T18 | `--keep-old-files` (`-k`) |
| T19 | Binary content integrity (md5) |
| T20 | 10 MB file round-trip |
| T21 | All 7 repo `src/` archives (list) |
| T22 | Hard link |
| T23 | Empty archive |
| T24 | V7 format |
| T25 | `--help` / `--version` |
| T26 | **AREGTYPE regression** — sidplay-2.0.9 extract + repack + re-extract + spot md5 |
| T27 | **Full round-trip** on all 7 repo archives (extract→repack→re-extract, file counts match) |
| T28 | `--newer` date filter |
| T29 | `--mtime` override |
| T30 | `--mode` override |
| T31 | `--transform` / `--xform` name rewriting |
| T32 | `--ignore-zeros` / `-i` (concatenated archive) |
| T33 | `--totals` output |
| T34 | `--block-number` / `-R` |
| T35 | `--remove-files` |
| T36 | `--atime-preserve` |
| T37 | `--record-size` → blocking factor |
| T38 | `--one-top-level` |
| T39 | `--to-command` |
| T40 | Sparse file write + extract round-trip |
| T41 | Hard link deduplication (nlink>1) |

Set `MUTAR_SRC_DIR=/path/to/archives` to point tests at a different `src/` directory.
Run against any tar-compatible tool via `TAR=mytool bash tests/run_tests.sh  # optional system tar path`.

---

## Source Layout

```
mutar/
├── CMakeLists.txt                    # CMake build (Debug/Release)
├── README.md                         # This file
├── ARCHITECTURE.md                   # Design notes
├── COMPATIBILITY_PROGRESS.md         # Option-by-option audit & PR changelog
├── src/
│   ├── mutar.hpp                      # Block layout, types, Config, Entry, helpers
│   └── mutar.cpp                      # Full implementation (~4000 lines)
└── tests/
    ├── run_tests.sh                  # 41-test core harness
    ├── test_formats_compression.sh   # 86-test format/compression suite
    ├── test_sparse.sh                # 23-test sparse file suite
    └── test_new_options.sh           # 20-test PR #170 options suite
```

Source reference material: GNU tar 1.35 `src/`, `lib/`, `rmt/`, `scripts/`
(extracted to `/tmp/tar-1.35/`).

---

## License

GPL-3.0-or-later (matching GNU tar).

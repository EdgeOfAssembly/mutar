# ARCHITECTURE.md — star C++23 design notes

## Overview

`star` is a single-translation-unit C++23 implementation (~1700 LOC across
`star.hpp` + `star.cpp`). It is structured in clean layers with no external
dependencies beyond POSIX and the C++23 standard library.

## Layer Stack

```
CLI (parse_args)
      ↓
Operations (op_create / op_extract / op_list / op_append / op_update / op_delete / op_diff / op_cat / op_test_label)
      ↓
ArchiveWriter / ArchiveReader
      ↓
BlockBuffer  (record-aligned I/O, blocking factor)
      ↓
ArchiveStream  (fd + optional compression child process)
```

## Key Types (star.hpp)

| Type | Purpose |
|------|---------|
| `Block` (union) | Raw 512-byte block; overlays `PosixHeader`, `OldgnuHeader`, `SparseHeader` |
| `Entry` | Decoded in-memory representation of one archive member |
| `Config` | All parsed CLI options (mirrors `common.h` globals in GNU tar) |
| `Format` | `V7 / OldGNU / USTAR / GNU / PAX` |
| `Operation` | `Create / Extract / List / Append / Update / Delete / Diff / Cat / TestLabel` |
| `Compress` | `None / Auto / Gzip / Bzip2 / Xz / Zstd / Lzma / Lzip / Lzop / Custom` |
| `Result<T>` | `std::expected<T, Error>` — used for fallible I/O returns |

## Block Layout

Mirrors `src/tar.h` in GNU tar 1.35 exactly:

```
Offset  Field         Notes
  0     name[100]     POSIX + V7
100     mode[8]
108     uid[8]        base-256 for >0777777
116     gid[8]        base-256 for >0777777
124     size[12]      base-256 for >8 GiB
136     mtime[12]
148     chksum[8]     sum of all bytes (chksum treated as spaces)
156     typeflag      '\0'=AREGTYPE (old V7 regular), '0'=REGTYPE (modern regular),
                      '1'-'7' special types, 'D'/'K'/'L'/'M'/'S'/'V'/'x'/'g' GNU/PAX
```

### AREGTYPE vs REGTYPE — Critical Distinction

`typeflag == '\0'` (`AREGTYPE`) is the **old V7 / early GNU tar** encoding for
a regular file. It is **not** a directory. The only time an `AREGTYPE` entry
should be treated as a directory is when the name ends with `/` (old implicit-
directory convention used by very early implementations).

Many real-world archives use `AREGTYPE` for every regular file:
`sidplay-2.0.9.tar.gz`, `dosbox-0.74-3.tar.gz`, `cbmfs-1.1.tar.gz`, etc.

The extraction `switch` must **not** group `case 0:` with `case DIRTYPE:` —
that silently `mkdir`s every regular file, yielding zero-file extractions and
near-empty re-packed archives (the critical bug fixed in PR #168).

Correct dispatch in `op_extract()`:

```
case DIRTYPE:                                → always mkdir
default (REGTYPE / AREGTYPE / CONTTYPE):
    if AREGTYPE && name ends with '/'        → old implicit-dir → mkdir
    else                                     → regular file extraction
```

After this fix all seven repo source archives (`sidplay`, `sidplay-libs`,
`dosbox`, `cbmfs`, `iir1`, `capstone`, `tar-1.35`) pass the full
extract → repack → re-extract round-trip with identical file counts.

---
157     linkname[100]
257     magic[6]      "ustar\0" (POSIX) or "ustar  " (GNU)
263     version[2]    "00" (POSIX)
265     uname[32]
297     gname[32]
329     devmajor[8]
337     devminor[8]
345     prefix[155]   POSIX path prefix (overloaded by oldgnu for atime/sparse)
500     (padding to 512)
```

## Compression Model

Compression is implemented via `fork()` + `exec()` of the external program
(`gzip`, `bzip2`, `xz`, `zstd`, …) in a child process piped to/from the
archive fd. This exactly mirrors GNU tar's approach in `src/system.c`.

Auto-detection on read tries filename extension first, then magic bytes
(`pread` at offset 0).

## PAX Extended Headers

PAX records use the standard `"N key=value\n"` format where N is the total
record length. Length convergence uses a simple iterative loop (at most 2
iterations since digit count can increase by at most 1):

```cpp
std::size_t len = base;
for (;;) {
    auto s = std::to_string(len);
    if (s.size() + base == len) break;
    len = s.size() + base;
}
```

## Path Normalization

Archive member names are stored **without** a leading `./` prefix (e.g.
`dir1/file2.txt`, not `./dir1/file2.txt`). On extraction the `want` set
normalizes user-supplied names the same way via `normalize_member()`, so
both `./file1.txt` and `file1.txt` match the stored name `file1.txt`.

## GNU Long Names / Long Links

For GNU format, names > 100 bytes are preceded by a synthetic `'L'`
(LongName) or `'K'` (LongLink) entry whose data is the full name. On read,
`ArchiveReader::next_entry()` accumulates these and applies them to the
next real header automatically.

## Sparse File Handling (PR #168)

### Write
`write_sparse()` in `ArchiveWriter`:
1. Opens file and calls `detect_sparse_segments(fd, size)` using `SEEK_DATA`/`SEEK_HOLE`.
2. If the entire file is data (no holes), falls back to `write_regular()`.
3. Otherwise builds the in-header sparse map (up to 4 entries in `oldgnu.sp[]`) plus extension sparse header blocks (21 entries each, `isextended=1` chain).
4. Writes only the data segments — compressed archive size matches actual data, not logical file size.
5. Restores atime if `--atime-preserve` is set.

`detect_sparse_segments()` falls back to a single `{0, file_size}` segment on kernels without `SEEK_DATA` or filesystems that don't support it.

### Extract
`extract_sparse_data()` in `ArchiveReader`:
1. `ftruncate(fd, real_size)` creates the full logical-size sparse file with holes.
2. Iterates through `e.sparse_map`, seeking to each segment's `offset` and writing its `numbytes` of archived data.
3. Bytes between segments stay as holes (zero pages, allocated on demand by the kernel).

## Hard Link Detection (PR #168)

In `op_create()`, for each path visited by `walk_dir()`:
1. If the file is a regular file with `st_nlink > 1`, compute key `(st_dev, st_ino)`.
2. If key found in `seen_inodes` map → emit a `LNKTYPE` ('1') entry with `size=0` and `linkname = first_seen_archname`.
3. Otherwise insert into `seen_inodes` and proceed with normal `add_path()`.

On extract, `LNKTYPE` entries are processed via `::link(link_target, outpath)`.
Verbose listing shows `h` for LNKTYPE entries (matching GNU tar output).

## New Helper Functions (PR #168)

| Function | Purpose |
|----------|---------|
| `parse_date_string(s)` | Parses dates for `--newer`/`--mtime`: file path → mtime, ISO 8601, human-readable, epoch seconds; handles `Z` suffix via `timegm()` |
| `apply_transform(name, expr)` | Applies `s/pat/rep/[flags]` expressions (regex via `<regex>`), multiple separated by `;`, flags `g`=global, `i`=case-insensitive |
| `detect_sparse_segments(fd, size)` | SEEK_DATA/SEEK_HOLE hole detection; falls back to single segment |
| `walk_dir(…, same_dev)` | Recursive directory walker; `same_dev` enables `--one-file-system` |

## --ignore-zeros (PR #168)

`ArchiveReader` accepts `bool ignore_zeros` in its constructor. When set, the
`next_entry()` loop uses `continue` on zero blocks instead of the EOF path,
enabling transparent reading of concatenated archives.

## --to-command (PR #168)

On extract, if `cfg.to_command` is non-empty, each regular file's data is piped
to a shell command via `fork()` + `pipe()` + `dup2(STDIN_FILENO)` rather than
written to disk. Environment variables `TAR_FILENAME`, `TAR_SIZE`, and
`TAR_REALNAME` are set for the child process.

## Exclude Pattern Matching

`--exclude=PATTERN` with no `/` in the pattern matches any **path component**
(basename-style), not just the full path. This mirrors GNU tar's behaviour
where `--exclude='*.o'` excludes `build/foo/bar.o`.

## C++23 Features Used

| Feature | Where |
|---------|-------|
| `std::expected<T,E>` | `Result<T>` return type for I/O operations |
| `std::format` | All string formatting (with GCC-13 shim for `std::print`) |
| `std::ranges::sort` | Directory entry sorting (`--sort=name`) |
| Structured bindings | `auto [e, ok, eof] = reader.next_entry()` |
| `std::string_view` | Zero-copy header field access throughout |
| `std::span` | Block buffer views |
| `[[nodiscard]]` | All header structs and `Result<T>` |
| Designated initialisers | `timespec` fields in `atime-preserve` calls |

## Build Flags (Debug)

Per `ubuntu-gdb-max-debug-best-practices.txt`:

```
-ggdb3 -O0 -gdwarf-5 -fvar-tracking-assignments -fvar-tracking
-fno-omit-frame-pointer -fstack-protector-all -pipe
```

DWARF 5 + full variable tracking gives the richest possible GDB experience.
The `pax_append` infinite-loop bug (infinite `while` before the convergence
`for(;;)`) was caught in under 60 seconds by attaching GDB to the hanging
process and reading `#1 in star::pax_append` directly from the backtrace.

## Known Gaps (Future PRs)

- **xattrs / ACLs / SELinux**: flag parsed, hooks present, not stored
- **Incremental backups** (`-G -g`): `--level=0` done; full snapshot state file not yet maintained
- **Remote tape** (`--rsh/rmt-command`): `rmt` protocol not wired
- **Multi-volume** (`-M`): volume switching not implemented
- **PAX sparse write**: sparse data currently written as GNU 'S' format only; PAX extended-header sparse map format not yet emitted
- **`--pax-option`**: parsed and stored in `Config`, not applied to PAX keyword generation
- **UID/GID map files**: `--owner-map` / `--group-map` stored but mapping not applied

---

## Changes in PR #170

### New `Config` Fields (star.hpp)

PR #170 added the following fields to the `Config` struct:

| Field | Type | Purpose |
|-------|------|---------|
| `verify` | `bool` | `--verify` / `-W` — trigger post-create re-read |
| `interactive` | `bool` | `--interactive` / `--confirmation` — per-file prompts |
| `show_transformed_names` | `bool` | `--show-transformed-names` — print original → new name |
| `overwrite_dir` | `bool` | `--overwrite-dir` — replace existing directories on extract |
| `preserve_order` | `bool` | `-s` / `--preserve-order` — skip sorting in create |
| `full_time` | `bool` | `--full-time` — nanosecond timestamps in `--list` |
| `hole_detection` | `std::string` | `--hole-detection=seek\|raw` — sparse detection mode |
| `level` | `int` | `--level=N` — incremental level (0 = full snapshot) |
| `index_file` | `std::string` | `--index-file=FILE` — path for index output |
| `checkpoint_action` | `std::string` | `--checkpoint-action=ACT` — `dot`, `echo`, `ttyout` |
| `warning_set` | `std::unordered_set<std::string>` | `--warning=KW` — active warnings |
| `no_warning_set` | `std::unordered_set<std::string>` | `--warning=no-KW` — suppressed warnings |
| `anchored` | `bool` | `--anchored` — exclude pattern anchored to path start |
| `ignore_case` | `bool` | `--ignore-case` — case-insensitive exclude matching |
| `wildcards_match_slash` | `bool` | `--wildcards-match-slash` — `*` matches `/` in patterns |
| `exclude_vcs_ignores` | `bool` | `--exclude-vcs-ignores` — load `.gitignore` etc. |
| `vcs_ignore_patterns` | `std::vector<std::string>` | Patterns loaded from VCS ignore files |

### New Behavioural Implementations

#### `is_excluded()` rewrite

The `is_excluded(path, cfg)` function was rewritten to honour the three new
pattern-matching flags from `Config`:

- **`cfg.anchored`** (default: `false`): when true, the pattern is matched
  against the full path only. When false (the GNU tar default), patterns are
  also matched against each path component (basename matching).
- **`cfg.ignore_case`**: passes `FNM_CASEFOLD` to `fnmatch` when wildcards are
  enabled, and uses `strcasecmp` for literal matches (`<strings.h>` required).
- **`cfg.wildcards_match_slash`** (default: `true`): when false, passes
  `FNM_PATHNAME` to `fnmatch` so `*` cannot cross `/` boundaries.

The same function also consults per-directory VCS ignore patterns (populated by
`--exclude-vcs-ignores`) alongside the explicit `cfg.excludes` list.

#### `do_checkpoint()` addition

A new `do_checkpoint(cfg, n)` helper is called every `cfg.checkpoint` entries
during create/extract/list. The `cfg.checkpoint_action` field selects output:

| Action | Behaviour |
|--------|-----------|
| *(empty)* | Prints `star: checkpoint N` to stderr (default) |
| `dot` or `.` | Prints `.` to stderr with no newline |
| `echo MESSAGE` | Prints `star: MESSAGE` to stderr |
| `ttyout=FORMAT` | Writes FORMAT to `/dev/tty` |

#### `g_index_fp` global mechanism

A file-scope `FILE* g_index_fp` pointer is opened at startup when
`cfg.index_file` is non-empty. When set, verbose output is routed
**exclusively** to the index file (not also to stdout), matching GNU tar's
documented `--index-file` behavior. All three major operations (`op_create`,
`op_extract`, `op_list`) route verbose member names through this pointer.

#### Verify mechanism in `op_create()`

After the archive file is written and the compression child process exits,
`op_create()` re-opens the archive in read mode and runs a full `ArchiveReader`
pass when `cfg.verify` is true. Each entry is checked for a valid header
checksum. Any mismatch is reported as a fatal error. This matches GNU tar's
`--verify` / `-W` semantics.

#### `detect_sparse_segments()` `hole_detection` param

`detect_sparse_segments(fd, size, hole_detection)` now takes an explicit
`std::string_view hole_detection` argument (from `cfg.hole_detection`):

- `"seek"` (default): uses `lseek(fd, off, SEEK_DATA)` / `SEEK_HOLE` — kernel
  detects holes with O(1) seeks, best for large sparse files.
- `"raw"`: reads the file in 512-byte blocks and treats runs of all-zero blocks
  as holes — filesystem-independent but slower for large files.
- Empty string: falls back to `"seek"` with `"raw"` as fallback if the kernel
  returns `ENXIO`.

Passing `--hole-detection` also sets `cfg.sparse = true` (implies `-S`).

#### `--level=0` incremental support

When `cfg.level == 0`, `op_create()` truncates the snapshot file (listed-incremental)
before writing the archive. Higher levels (`--level=N, N>0`) are stored in
`Config` but the incremental snapshot-file state machine is not yet
implemented.

#### `--exclude-vcs-ignores` implementation

During `walk_dir()`, when `cfg.exclude_vcs_ignores` is true, the walker
searches each directory for `.gitignore`, `.hgignore`, `.cvsignore`, and
`.bzrignore` files. Each non-comment, non-empty line is used as an `fnmatch`
pattern for entries in that directory.

#### Partial / no-op options (PR #170)

The following options are **accepted and parsed but not yet behaviorally wired**:

| Option | Status |
|--------|--------|
| `-s` / `--preserve-order` | Flag stored; emits a "not implemented" warning; directory ordering is controlled only by `--sort` |
| `--overwrite-dir` / `--no-overwrite-dir` | ✅ PR #172: fully wired in DIRTYPE extraction case |
| `--warning=KEYWORD` | ✅ PR #172: `star_warn()` helper wired at key emission sites |

---

## PR #172 Architecture Changes

### Owner/group map pipeline

`load_id_map(path)` → `std::map<std::string,std::string>` (loaded once per
`op_create()`). `ArchiveWriter::set_owner_map(om, gm)` stores pointers to
these maps. Inside `add_path()`, `apply_owner_map(e, *owner_map_, *group_map_)`
is called after the uname/gname lookup and before encoding the header. This
keeps the mapping purely as a writer-level post-processing step.

### PAX sparse write path

`write_sparse()` now branches at the top on `fmt_ == Format::PAX`. The PAX
branch emits an `XHDTYPE ('x')` extended header containing `GNU.sparse.*` PAX
keywords, then a `REGTYPE` data header, then the concatenated sparse segment
data. The existing GNU `'S'` path is unchanged and used for all non-PAX modes.

On read, `apply_pax_attrs()` parses `GNU.sparse.map` into `e.sparse_map`, so
PAX-sparse archives created by other tools (GNU tar 1.35 in pax mode) can be
extracted correctly.

### Incremental snapshot state machine

`op_create()` now maintains:
- `snapshot_map`: read from the snapshot file when `level >= 1`.
- `snapshot_entries`: populated by the `add_file` lambda for every archived file.
- After `writer.finish()`: writes updated snapshot atomically via `mkstemp` +
  `rename`.

The snapshot format is line-oriented (`name<TAB>mtime_sec`) with a
`STAR_SNAPSHOT_V1` header, making it human-readable and grep-friendly.

### Remote archive (rmt) bridge

`is_remote_archive()` detects `[user@]host:path` in `cfg.archive_file`.
`open_remote_stream()` forks a bridge child that:
1. Forks `rsh [user@]host rmt` as a sub-child.
2. Sends `O path mode\n` and reads the `A 0\n` response.
3. For write: reads raw bytes from a pipe, sends `W count\ndata`, reads `A`.
4. For read: sends `R count\n`, reads `A len\ndata`, writes to a pipe.
5. Sends `C\n` on EOF/close.

The pipe fd returned becomes `ArchiveStream::fd_`, so the rest of the code
sees it as a normal byte stream. The bridge child is tracked like the
compression child (reused `child_pid_` sentinel `-2` for remote bridges that
don't require `waitpid`).

### Warning emission system

`star_warn(cfg, category, msg)` is a free function that:
1. Returns immediately if `cfg.warn_none`.
2. Emits unconditionally if `cfg.warn_all`.
3. Checks `warnings_disabled` (set) — if `category` is in the set, suppress.
4. Emits if `warnings_enabled` is empty (default) or contains `category`.

Existing `print(stderr, ...)` warning sites were replaced with `star_warn(...)`
at: walk_dir lstat failure (`failed-read`), --newer filter skip (`newer`),
--check-links missing link (`missing-links`), --one-file-system skip (`xdev`).

### `--overwrite-dir` / `--no-overwrite-dir`

In the `DIRTYPE` case of `op_extract()`, `lstat()` is called on the target
path before `mkdir()`. If the path already exists as a directory:
- `--no-overwrite-dir`: break immediately after `mkdir` (no fixup entry).
- `--overwrite-dir` (default): proceed to add to `dir_fixups` as before.
If the path exists as a non-directory, an error is printed and the entry is
skipped. `OPT_OVERWRITE_DIR` and `OPT_NO_OVERWRITE_DIR` clear each other's flag.

#### Comprehensive `--help` output

The `print_usage()` function was rewritten from a short stub to enumerate all
~100 parsed options, grouped by 13 categories (Main operations, Compression,
Format, File selection, Extraction, Attributes, Informational, Compatibility).
This matches the information density of `tar --help` in GNU tar 1.35.

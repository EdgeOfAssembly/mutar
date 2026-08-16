# mutar (µtar) — Compatibility Progress & PR #170 Changelog

**Compatibility goal: ~99% GNU tar 1.35. SELinux is unsupported.**

This document is the proof-of-work record for PR #170. It captures the
complete option-by-option audit against `tar(1)` / GNU tar 1.35, describes
every change made in this PR, records test results, and honestly lists what
remains unimplemented.

---

## Environment

Compression tools available in the CI / development environment:

| Tool | Available | Notes |
|------|-----------|-------|
| `gzip` | ✅ | Full round-trip tests pass |
| `bzip2` | ✅ | Full round-trip tests pass |
| `xz` | ✅ | Full round-trip tests pass |
| `zstd` | ✅ | Full round-trip tests pass |
| `lzma` | ✅ | Full round-trip tests pass |
| `lzip` | ❌ | Not installed — related tests skip |
| `lzop` | ❌ | Not installed — related tests skip |
| `compress` (`.Z`) | ❌ | Not installed — related tests skip |

---

## Audit: tar.1 vs mutar.cpp

Legend:
- ✅ **Implemented** — option is parsed and has full behavioural effect
- ⚠️ **Partial** — option is parsed and stored in `Config` but effect is incomplete
- ❌ **Not implemented** — option is not parsed or is silently ignored
- 🔧 **Wired PR#170** — option existed before but was only stored; PR #170 wired it to actual behaviour
- 🆕 **New PR#170** — option was a no-op stub or missing; PR #170 fully implemented it

### Main Operations

| Option | Status | Notes |
|--------|--------|-------|
| `-c` / `--create` | ✅ | |
| `-x` / `--extract` / `--get` | ✅ | |
| `-t` / `--list` | ✅ | |
| `-r` / `--append` | ✅ | |
| `-u` / `--update` | ✅ | |
| `-A` / `--catenate` / `--concatenate` | ✅ | |
| `-d` / `--diff` / `--compare` | ✅ | |
| `--delete` | ✅ | |
| `--test-label` | ✅ | |

### Archive File

| Option | Status | Notes |
|--------|--------|-------|
| `-f` / `--file=ARCHIVE` | ✅ | |
| `-` (stdin/stdout) | ✅ | |
| `-b` / `--blocking-factor=N` | ✅ | |
| `--record-size=SIZE` | ✅ | |
| `-i` / `--ignore-zeros` | ✅ | |
| `-B` / `--read-full-records` | ✅ | |

### Compression

| Option | Status | Notes |
|--------|--------|-------|
| `-z` / `--gzip` / `--gunzip` | ✅ | |
| `-j` / `--bzip2` | ✅ | |
| `-J` / `--xz` | ✅ | |
| `--zstd` | ✅ | |
| `--lzma` | ✅ | |
| `--lzip` | ✅ | Tool may be absent on host |
| `--lzop` | ✅ | Tool may be absent on host |
| `-Z` / `--compress` / `--uncompress` | ✅ | Tool may be absent on host |
| `--use-compress-program=PROG` | ✅ | |
| `-a` / `--auto-compress` | ✅ | |
| `--no-auto-compress` | ✅ | |

### Archive Format

| Option | Status | Notes |
|--------|--------|-------|
| `-H` / `--format=FORMAT` | ✅ | v7, oldgnu, gnu, ustar, pax |
| `--posix` | ✅ | Alias for `-H pax` |
| `--old-archive` / `--portability` | ✅ | Alias for `-H v7` |
| `--pax-option=KW[:=]VAL` | ⚠️ Partial | `delete=KEYWORD` applied when writing PAX headers; other keywords accepted silently (not full GNU set) |

### File Selection

| Option | Status | Notes |
|--------|--------|-------|
| `-T` / `--files-from=FILE` | ✅ | |
| `--null` / `--no-null` | ✅ | NUL-separated file lists |
| `-X` / `--exclude-from=FILE` | ✅ | |
| `--exclude=PATTERN` | ✅ | |
| `--exclude-vcs` | ✅ | Skips `.git`, `.hg`, `.svn`, etc. |
| `--exclude-backups` | ✅ | Skips `*~`, `#*#`, etc. |
| `--exclude-vcs-ignores` | 🆕 | Reads `.gitignore`, `.hgignore`, `.cvsignore`, `.bzrignore` |
| `-N` / `--newer=DATE` / `--after-date=DATE` | ✅ | |
| `--newer-mtime=DATE` | ✅ | |
| `--anchored` / `--no-anchored` | 🔧 | Wired into `is_excluded()` |
| `--ignore-case` / `--no-ignore-case` | 🔧 | Wired into `is_excluded()` |
| `--wildcards-match-slash` / `--no-wildcards-match-slash` | 🔧 | Wired into `is_excluded()` |
| `--wildcards` / `--no-wildcards` | ✅ | |
| `--one-file-system` / `-l` | ✅ | |

### Path Handling

| Option | Status | Notes |
|--------|--------|-------|
| `-P` / `--absolute-names` | ✅ | |
| `--strip-components=N` | ✅ | |
| `--transform=EXPR` / `--xform=EXPR` | ✅ | |
| `--show-transformed-names` / `--show-stored-names` | 🔧 | Wired during create |

### File Attributes

| Option | Status | Notes |
|--------|--------|-------|
| `--owner=NAME` | ✅ | |
| `--group=NAME` | ✅ | |
| `--owner-map=FILE` | ✅ | PR #172: map file parsed; uname/uid remapped at archive time |
| `--group-map=FILE` | ✅ | PR #172: map file parsed; gname/gid remapped at archive time |
| `--mtime=DATE` | ✅ | |
| `--mode=MODE` | ✅ | |
| `--numeric-owner` | ✅ | |
| `--same-owner` / `--no-same-owner` | ✅ | |
| `-p` / `--preserve-permissions` / `--same-permissions` | ✅ | |
| `--no-same-permissions` | ✅ | |
| `-m` / `--touch` | ✅ | |
| `--atime-preserve[=METHOD]` | ✅ | |
| `--utc` | ✅ | |
| `--full-time` | 🔧 | Nanosecond timestamps now shown in `--list` |
| `--xattrs` / `--no-xattrs` | ✅ | When `MUTAR_HAVE_XATTR`: create stores via `llistxattr`/`lgetxattr` as PAX `SCHILY.xattr.*` (raw values, GNU interop); extract restores with `lsetxattr`. Create+restore skip privileged namespaces `security.*`, `trusted.*`, `system.*` (ACLs via `--acls`). |
| `--xattrs-include=MASK` / `--xattrs-exclude=MASK` | ✅ | fnmatch filters applied on store; include empty ⇒ all non-skipped keys; exclude always applied. |
| `--acls` / `--no-acls` | ✅ | When `MUTAR_HAVE_ACL`: create stores non-trivial access ACL + default ACL (dirs) as `SCHILY.acl.access` / `SCHILY.acl.default` text; extract `acl_from_text` + `acl_set_file`. |
| `--selinux` / `--no-selinux` | ❌ Unsupported | Project policy: no SELinux test hardware. Options accepted as no-ops with warning. Never stored even with `--xattrs`. |

### Extraction Behaviour

| Option | Status | Notes |
|--------|--------|-------|
| `-C` / `--directory=DIR` | ✅ | |
| `-O` / `--to-stdout` | ✅ | |
| `--to-command=CMD` | ✅ | |
| `--one-top-level[=DIR]` | ✅ | |
| `-k` / `--keep-old-files` | ✅ | |
| `--skip-old-files` | ✅ | |
| `--keep-newer-files` | ✅ | |
| `--overwrite` | ✅ | |
| `--overwrite-dir` | ✅ | PR #172: DIRTYPE extract respects flag (default on) |
| `-U` / `--unlink-first` | ✅ | |
| `--recursive-unlink` | ✅ | |
| `--no-overwrite-dir` | ✅ | PR #172: skips dir mtime/mode fixup when target exists |
| `-s` / `--preserve-order` / `--same-order` | ⚠️ | Accepted; emits not-implemented warning; not wired into traversal |
| `-p` / `--preserve` | ✅ | |
| `--delay-directory-restore` | ✅ | |

### Sparse Files

| Option | Status | Notes |
|--------|--------|-------|
| `-S` / `--sparse` | ✅ | GNU 'S' format |
| `--sparse-version=M.N` | ⚠️ | String stored only; write path hardcodes GNU.sparse 1.0 (major/minor never parsed from CLI) |
| `--hole-detection=METHOD` | 🔧 | `seek` / `raw` wired into `detect_sparse_segments()` |

### Informational / Output

| Option | Status | Notes |
|--------|--------|-------|
| `-v` / `--verbose` | ✅ | |
| `-R` / `--block-number` | ✅ | |
| `--totals[=SIGNAL]` | ✅ | |
| `--utc` | ✅ | |
| `--index-file=FILE` | 🆕 | Now opens and writes during create/list/extract |
| `--show-omitted-dirs` | 🔧 | Wired into create directory walk |
| `--show-transformed-names` | 🔧 | Wired into create transform |
| `--checkpoint[=N]` | ✅ | |
| `--checkpoint-action=ACTION` | 🆕 | `dot/echo/ttyout` via `do_checkpoint()` |
| `--warning=KEYWORD` | 🆕 | Warning set processing implemented |
| `--restrict` | ✅ | Phase E: rejects `-P`, `--to-command`, multi-volume |
| `--quoting-style=STYLE` | ✅ | Phase E: applied on `-t` / verbose extract |

### Hard Links & Labels

| Option | Status | Notes |
|--------|--------|-------|
| `-V` / `--label=TEXT` | ✅ | |
| `--test-label` | ✅ | |

### Verification & Safety

| Option | Status | Notes |
|--------|--------|-------|
| `-W` / `--verify` | 🆕 | Post-create re-read verification implemented |
| `--interactive` / `--confirmation` | 🆕 | Per-file confirmation prompts |
| `--check-device` / `--no-check-device` | ✅ | Phase E: `check_device` default true; snapshot V2 device field |

### Incremental / Snapshot

| Option | Status | Notes |
|--------|--------|-------|
| `-G` / `--incremental` | ⚠️ | Parsed; old incremental format not maintained |
| `-g` / `--listed-incremental=FILE` | 🔧 | PR #172: snapshot file written on level-0; level≥1 reads snapshot and skips unchanged files |
| `--level=N` | 🔧 | PR #172: `--level=0` creates fresh snapshot; `--level=1` reads and uses snapshot for incremental |
| `--ignore-failed-read` | ✅ | |

### Multi-Volume

| Option | Status | Notes |
|--------|--------|-------|
| `-M` / `--multi-volume` | ⚠️ | Between-member create/extract with stream swap; mid-file split not supported |
| `-L` / `--tape-length=N` | ✅ | Parsed into `cfg.tape_length` (KiB); implies `-M`; pre-rotates when next member won't fit |
| `-F` / `--info-script=CMD` / `--new-volume-script=CMD` | ✅ | Exec'd at volume boundary; `TAR_ARCHIVE`/`TAR_VOLUME` set; non-zero exit fails create; implies `-M` |
| `--volno-file=FILE` | ✅ | Read at start; atomic write (mkstemp+rename) after each rotation and at end |

### Remote Archives

| Option | Status | Notes |
|--------|--------|-------|
| `--rsh-command=CMD` | 🔧 | PR #172: remote filename detection and rmt protocol bridge via rsh fork/pipe implemented |
| `--rmt-command=CMD` | 🔧 | PR #172: used with rsh bridge; rmt O/R/W/C protocol commands implemented |

### Sorting & Other

| Option | Status | Notes |
|--------|--------|-------|
| `--sort=ORDER` | ✅ | `name` implemented; `inode` accepted |
| `--remove-files` | ✅ | |
| `--backup[=CONTROL]` | ✅ | Phase E: `none`/`off`, `simple`/`never`, `numbered`/`t` (`file.~N~`), `existing`/`nil` |
| `--suffix=SUFFIX` | ✅ | Sets `backup_suffix` (default `~`); used by simple/existing fallback |
| `--help` | 🆕 | All ~100 options now listed |
| `--version` | ✅ | |
| `--usage` | ✅ | |

---

## Implemented in PR #170

### 1. `--verify` / `-W` — post-create verification

After `op_create()` closes the archive and the compression child exits, if
`cfg.verify` is set, the archive is re-opened in read mode and subjected to a
full `ArchiveReader` pass. Each entry header checksum is validated. Any mismatch
is a fatal error. This mirrors GNU tar's `--verify` semantics.

### 2. `--interactive` / `--confirmation` — per-file prompts

Before extracting each member, if `cfg.interactive` is set, the user is
prompted with `"extract <name>? [y/N] "`. Any response beginning with `y` or
`Y` proceeds; anything else skips the entry.

Confirmations are read from `/dev/tty` (falling back to `stdin` only if it
is a TTY). If neither is available (e.g. stdin is the archive pipe and no
`/dev/tty` is accessible), the entry is skipped.

### 3. `--warning=KEYWORD` processing

`parse_args()` now splits `--warning=KEYWORD` into `cfg.warnings_enabled` and
`--warning=no-KEYWORD` into `cfg.warnings_disabled` sets. The option is
accepted for GNU tar compatibility. **Currently the parsed sets are not wired
into any emission sites** — no `should_warn()` gating exists yet. This is a
known remaining gap.

### 4. Exclude pattern anchoring — `is_excluded()` rewrite

Three flags are now honoured by `is_excluded(path, cfg)`:

- `cfg.anchored` (default: `false`) — when true, match full path; when false
  (the GNU tar default), also match each path component
- `cfg.ignore_case` — case-insensitive glob/regex matching
- `cfg.wildcards_match_slash` (default: `true`) — when true, `*` can cross
  `/` boundaries; when false, adds `FNM_PATHNAME`

### 5. `--show-omitted-dirs` wired into create and list

During the `walk_dir()` recursive descent (create), when a directory is
excluded by any `is_excluded()` test, a `"mutar: <dir>/"` message is printed
to stderr when `cfg.show_omitted_dirs` is set.

During `op_list()`, when a directory entry doesn't match the requested members
(`want` set), it is printed to stderr as an omitted directory.

### 6. `--show-transformed-names` wired into create

After `apply_transform(name, cfg.transform)`, if `cfg.show_transformed` is
set and the name actually changed, a line `"<original> -> <new>"` is printed
to stderr.

### 7. `--index-file` fixed — exclusive routing

`g_index_fp` is now a file-scope `FILE*` opened at startup when
`cfg.index_file` is non-empty. When set, verbose output is routed
**exclusively to the index file** (not also to stdout). Previously the option
was stored but the pointer was never opened, and output went only to stdout.

### 8. `--checkpoint-action` via `do_checkpoint()`

A `do_checkpoint(cfg, n)` function is called every `cfg.checkpoint` processed
entries. The `cfg.checkpoint_action` value selects output:
- *(empty)* → `"mutar: checkpoint N\n"` to stderr (default behavior)
- `dot` or `.` → `.` to stderr (no newline)
- `echo MESSAGE` → `"mutar: MESSAGE\n"` to stderr
- `ttyout=FORMAT` → writes FORMAT to `/dev/tty`

### 9. `--full-time` nanosecond timestamps

When `cfg.full_time` is set, the listing path in `op_list()` formats the
mtime as `YYYY-MM-DD HH:MM:SS.NNNNNNNNN` instead of the default truncated form.

### 10. `-s` / `--preserve-order` (partial / no-op)

The `cfg.preserve_order` flag is parsed and stored. The option **emits a
"not implemented" warning** when used. It is **not yet consulted** by
`walk_dir()` or any other traversal code — directory ordering is controlled
solely by `cfg.sort_order`. This remains a known gap.

### 11. `--overwrite-dir` / `--no-overwrite-dir` (implemented)

`cfg.overwrite_dir` (default `true`) and `cfg.no_overwrite_dir` are mutually
exclusive. In `op_extract` DIRTYPE handling, an existing directory target skips
mtime/permission fixup when `--no-overwrite-dir` is set; otherwise fixup proceeds.

### 12. `--exclude-vcs-ignores` implemented (adds `.bzrignore`)

During `walk_dir()`, when `cfg.exclude_vcs_ignores` is true, each traversed
directory is checked for `.gitignore`, `.hgignore`, `.cvsignore`, and
`.bzrignore`. Non-comment, non-empty lines are used as `fnmatch` patterns
for entries in that directory.

### 13. `--hole-detection=seek/raw` wired (implies `--sparse`)

`detect_sparse_segments(fd, size)` now accepts a `hole_detection` string:
- `"seek"` — uses `SEEK_DATA`/`SEEK_HOLE` lseek (default, fast)
- `"raw"` — reads 512-byte blocks and treats zero runs as holes (slower,
  portable)
- empty — tries `"seek"` first; falls back to `"raw"` on `ENXIO`

### 14. `--level=0` implemented

When `cfg.level == 0`, `op_create()` prepends a PAX global extended header
(`g`-type entry, member name `././@GlobalHeader`) containing a
`GNU.dumpdir=` record before the first file entry.

### 15. Comprehensive `--help`

`print_help()` was rewritten to enumerate all ~100 parsed options, grouped
into sections matching GNU tar 1.35's `--help` output structure.

---

## Test Results

All results measured in the CI environment described in the Environment section.

### CTest

```
ctest --output-on-failure
Test project …/mutar/build
    Start 1: mutar_tests          Passed
    Start 2: mutar_shell_tests    Passed
2/2 tests passed
```

### `tests/run_tests.sh` (core harness)

```
bash tests/run_tests.sh build/mutar
…
Tests passed: 41 / 41
Tests failed: 0
```

### `tests/test_formats_compression.sh`

```
bash tests/test_formats_compression.sh build/mutar
…
Passed: 59   Skipped: 27   Failed: 0
```

The 27 skipped tests are for `lzip`, `lzop`, and `compress` — tools not
present in the CI environment. All skips are expected and benign.

### `tests/test_sparse.sh`

```
bash tests/test_sparse.sh build/mutar
…
Passed: 20   Skipped: 3   Failed: 0
```

The 3 skipped tests require a filesystem that supports sparse files via
`SEEK_DATA`/`SEEK_HOLE`; they are skipped on overlayfs/tmpfs CI runners.

### `tests/test_new_options.sh` (PR #170)

```
bash tests/test_new_options.sh build/mutar
…
Passed: 20   Failed: 0
```

### `tests/test_pr172_features.sh` (PR #172)

```
bash tests/test_pr172_features.sh build/mutar
…
Results: 10 passed, 0 failed, 0 skipped
```

---

## Implemented in PR #172

### 1. `--owner-map=FILE` / `--group-map=FILE` — owner/group remapping

`load_id_map()` parses files of the form `OLD NEW` (comment lines with `#`
skipped). `apply_owner_map()` applies the map to an `Entry`'s `uname`, `gname`,
`uid`, `gid` before writing the header. `ArchiveWriter` stores optional map
pointers and applies them inside `add_path()`. Both name-based and numeric-ID
keys are supported.

### 2. PAX sparse write format (`--format=pax -S`)

`write_sparse()` now checks `fmt_ == Format::PAX`. If true, it emits:
- A PAX `'x'` extended header with `GNU.sparse.major=1`, `GNU.sparse.minor=0`,
  `GNU.sparse.realsize`, `GNU.sparse.name`, and `GNU.sparse.map`.
- A regular `REGTYPE` data header with `size = archived_bytes`.
- The concatenated sparse segment data.

`apply_pax_attrs()` parses `GNU.sparse.map` (`offset,length,...`) into
`e.sparse_map` on read.

### 3. Incremental snapshot (`-g FILE --level=N`)

On level-0 (or when no snapshot exists): archives all files, then writes
`MUTAR_SNAPSHOT_V1\nname\tmtime\n...` via `mkstemp` + `rename` (atomic).

On level≥1: reads the snapshot, and the `add_file` lambda skips regular
files whose mtime ≤ snapshot mtime. After archiving, the snapshot is updated
to include all new/modified entries.

**Limitation**: only regular files are compared by mtime. Directories,
symlinks, and special files are always archived on level≥1.

### 4. Multi-volume support (`-M --tape-length=N`) — between-member (GOAL_NEXT Phase C)

`-L` / `--tape-length=N` parses into `cfg.tape_length` (N × 1024 bytes) and
implies `-M`. `make_volume_name()` uses `%d` or `.N` suffixes. On create,
before each member, if the estimated size will not fit on the current volume,
`rotate_volume()` finishes the volume (EOF blocks), runs `--info-script` (with
`TAR_ARCHIVE` / `TAR_VOLUME`), writes `--volno-file` atomically, opens the next
volume path, and `ArchiveWriter::swap_stream()` continues. Extract on EOF
auto-opens the next volume file when present and `ArchiveReader::swap_stream()`
continues.

**Limitation (⚠️ Partial for G2 mid-file):** a single member larger than the tape
length is rejected with a clear error — mid-file split (`GNUTYPE_MULTIVOL` 'M')
is not implemented. Tests: `tests/test_multi_volume.sh`.

### 4b. xattrs + ACLs (GOAL_NEXT Phase D / G6–G8)

When built with `MUTAR_HAVE_XATTR` / `MUTAR_HAVE_ACL` (CMake detection):

- **Create (`--xattrs`)**: `llistxattr` + `lgetxattr`; emit PAX records
  `SCHILY.xattr.<key>=<raw-value>` (GNU tar interop; keyword percent-encodes
  `=` / `%` only). Skips privileged namespaces `security.*`, `trusted.*`,
  `system.*` (POSIX ACLs via `--acls` → `SCHILY.acl.*`).
  `--xattrs-include` / `--xattrs-exclude` use `fnmatch`.
- **Create (`--acls`)**: `acl_get_file` ACCESS (+ DEFAULT for dirs);
  non-trivial access ACLs and default ACLs stored as `SCHILY.acl.access` /
  `SCHILY.acl.default` text (`acl_to_text`).
- **Extract**: with matching flags, `lsetxattr` / `acl_from_text`+`acl_set_file`
  after the file/dir is created. Restore also skips privileged xattr namespaces.
  Without the flag, SCHILY records are ignored.
- **SELinux (G9)**: still unsupported; `--selinux` no-op + warning; never stored.
- **Extract open safety**: path walk via `openat`/`mkdirat` with `O_NOFOLLOW`;
  intermediate directory symlinks are replaced with real dirs (unless
  `--keep-directory-symlink`); final-component symlink is unlinked before
  regular write (same-name and dir-nested zip-slip). PAX numeric fields
  (`stoll`/`stod`) are try/catch-guarded. Materialize temp uses RAII unlink.
  `make_volume_name` substitutes the first `%d` without printf format strings.

Tests: `tests/test_xattrs_acls.sh` (CTest `mutar_xattrs_acls_tests`); skips when
libs/tools/FS lack support. Extract safety: `tests/test_extract_safety.sh`
(CTest `mutar_extract_safety_tests`).

### 5. Remote tape / rmt (`--rsh-command` / `--rmt-command`)

`is_remote_archive()` detects `[user@]host:path` syntax. `open_remote_stream()`
forks a bridge child that runs `rsh host rmt`, opens the remote file with
`O path mode\n`, then bridges raw I/O via `W count\ndata` (write) or
`R count\n` + `A len\ndata` (read) rmt commands. The bridge fd becomes the
ArchiveStream's fd, making it transparent to the rest of the code.

**Limitation**: `lseek` over rmt (the `S` command) is not implemented.
Append/update operations on remote archives are not supported.

### 6. `--warning=KEYWORD` wiring

`mutar_warn(cfg, category, msg)` checks `cfg.warn_none`, `cfg.warn_all`,
`warnings_disabled`, and `warnings_enabled` before emitting to stderr. Wired
at: `failed-read` (lstat failures in walk_dir), `newer` (--newer filter skips),
`missing-links` (--check-links), `xdev` (--one-file-system skips).

### 7. `--overwrite-dir` / `--no-overwrite-dir` semantics

In `op_extract`'s `DIRTYPE` case, if the target path already exists:
- If it's a non-directory: error and skip.
- If it's a directory and `--no-overwrite-dir`: skip adding to `dir_fixups`
  (no mtime/permission update).
- If it's a directory and `--overwrite-dir` (default): proceed normally.

`OPT_OVERWRITE_DIR` and `OPT_NO_OVERWRITE_DIR` are now mutually exclusive.

---

## Phase 5 — Sidecar index + seek (2026-08-16)

| Feature | Status | Notes |
|---------|--------|-------|
| `--write-index` | ✅ | Writes `ARCHIVE.mutaridx` (`MUTAR.INDEX.V1`) |
| `--mutar-index=FILE` | ✅ | Explicit path for write/read |
| Fast `-t` via index | ✅ | Non-verbose list from sidecar |
| Seek extract | ✅ | Uncompressed seekable archives; selective members |
| Compressed seek | ✅ | Phase 6: materialize-then-seek with index |

## Phase 6 — Seekable compression (2026-08-16)

| Feature | Status | Notes |
|---------|--------|-------|
| `--seekable` | ✅ | Implies `--write-index` |
| xz multi-block write | ✅ | `--block-size=1MiB` |
| zstd chunked write | ✅ | `-T0 -B1M` |
| gzip/bzip2 seek | ⚠️ | Warns; materialize-then-seek still works with index |
| Compressed selective extract | ✅ | **Materialize once** (full decompress to temp) + index seek — not frame-level |
| Uncompressed selective extract | ✅ | Direct `lseek`; **never** materializes when stream is seekable |
| Index comment on `--seekable` compress | ✅ | `# compressed=<prog> seekable=materialize` (informational) |
| True frame-level seek without materialize | ❌ | Future (liblzma/libzstd APIs); Phase F G16 documents materialize-only |

## Phase F — Seek/perf honesty (2026-08-16)

| Item | Status | Notes |
|------|--------|-------|
| G16 compressed seek docs | ✅ | ARCHITECTURE / help / this table: materialize-then-seek only |
| G17 micro-benchmarks | ✅ | `tests/bench_index_seek.sh`; numbers in `PROGRESS.md`; CTest `mutar_bench_smoke` (always pass) |
| G18 no false speed claims | ✅ | README: performance claims require published benchmarks |

## Known Remaining Gaps

The following features are accepted by the CLI parser but are not (or not
fully) implemented. They are noted here honestly to avoid overstating
compatibility.

| Feature | Config field(s) | Gap |
|---------|----------------|-----|
| `--pax-option` | `pax_options`, `pax_option_rules` | ⚠️ Partial: `delete=KEYWORD` (repeatable / comma-list) suppresses that keyword in `write_pax_header` and sparse PAX emission; other keywords ignored |
| `--volno-file` | `volno_file` | ✅ Phase C: CLI assigns; atomic read/write of current volume number |
| `--owner-map` / `--group-map` | `owner_map_file`, `group_map_file` | ✅ PR #172: fully implemented; maps uname/gname/uid/gid at create time |
| `--info-script` / `--new-volume-script` | `info_script` | ✅ Phase C: exec'd at volume boundary; non-zero fails; TAR_* env |
| `--check-device` / `--no-check-device` | `check_device` | ✅ Phase E: default true; snapshot V2 stores `st_dev`; re-archive when device changes under listed-incremental |
| `--restrict` | `restrict_opt` | ✅ Phase E: rejects `-P`/`--absolute-names`, `--to-command`, multi-volume |
| `--quoting-style` | `quoting_style` | ✅ Phase E: `literal`/`escape`/`c`/`c-maybe`/`shell`/`shell-always` for `-t` and verbose extract |
| `--backup` / `--suffix` | `backup`, `backup_control`, `backup_suffix` | ✅ Phase E: `none`/`off`, `simple`/`never`, `numbered`/`t`, `existing`/`nil` |
| `--sparse-version` | `sparse_version` | ⚠️ String stored; `sparse_major`/`sparse_minor` never parsed; write hardcodes 1.0 |
| `-L` / `--tape-length` | `tape_length_str`, `tape_length` | ✅ Phase C: numeric parse into `tape_length`; implies `-M` |
| `--xattrs` / `--acls` | `xattrs`, `acls` | ✅ Store/restore via SCHILY PAX (GOAL_NEXT Phase D / G6–G8); SELinux still unsupported |
| `--selinux` / `--no-selinux` | `selinux` | **Unsupported by policy** (no test hardware); accepted as no-op with warning |
| `-G` / `-g` (incremental) | `listed_incremental` | ⚠️ Phase E: snapshot **V2** (`name\tmtime\tdev`) records files **and directories**; level≥1 skip is still **regular-file mtime** (+ device when `--check-device`); dirs/symlinks/specials always archived |
| `-M` (multi-volume) | `multi_volume` | ⚠️ Phase C: between-member stream swap create+extract; **mid-file split still TODO** |
| `--rsh-command` / `--rmt-command` | `rsh_command`, `rmt_command` | 🔧 PR #172: rmt bridge via rsh fork+pipe; O/R/W/C protocol implemented. **Limitation (G11):** rmt `lseek`/`S` and remote append not implemented (documented in `--help`) |
| PAX sparse write format | `fmt_` | ✅ PR #172: when `--format=pax`, GNU.sparse.* PAX keywords emitted; GNU.sparse.map parsed on read |
| `--warning=KEYWORD` | `warnings_enabled/disabled` | ✅ PR #172: `mutar_warn()` helper implemented and wired at key emission sites |
| `--overwrite-dir` / `--no-overwrite-dir` | `overwrite_dir`, `no_overwrite_dir` | ✅ PR #172: DIRTYPE case in op_extract checks existing path and skips fixup when --no-overwrite-dir |

---

## Test Methodology

### How tests were run

All test suites are plain Bash scripts that take the path to the `mutar`
binary as their first argument. They do not require `root`; they use
`mktemp -d` for isolation and clean up on exit.

```bash
# From the mutar directory after building:
bash tests/run_tests.sh         build/mutar
bash tests/test_formats_compression.sh  build/mutar
bash tests/test_sparse.sh       build/mutar
bash tests/test_new_options.sh  build/mutar
bash tests/test_xattrs_acls.sh  build/mutar
```

### What the tests verify

- **`run_tests.sh`**: create/extract/list round-trips, interoperability with
  system `tar`, format correctness (magic bytes, header fields), regression
  for the AREGTYPE bug, hard links, sparse files, all 41 real-world
  behavioural options.

- **`test_formats_compression.sh`**: every `(format × compression)` matrix
  cell — creates a multi-file archive in each format with each available
  compressor, extracts it, and md5-verifies the contents. Skips gracefully
  when a compressor binary is absent.

- **`test_sparse.sh`**: creates files with known hole patterns, archives them
  with `--sparse`, extracts, and verifies that holes are preserved (sparse
  block count < dense block count). Also tests `--sparse-version` variants.

- **`test_new_options.sh`**: targeted tests for each option introduced or
  wired in PR #170: `--verify`, `--interactive` (automated via `yes`),
  `--warning`, `--anchored`, `--ignore-case`, `--wildcards-match-slash`,
  `--show-omitted-dirs`, `--show-transformed-names`, `--index-file`,
  `--checkpoint-action`, `--full-time`, `--preserve-order`,
  `--overwrite-dir`, `--exclude-vcs-ignores`, `--hole-detection`,
  `--level=0`, and `--help` completeness.

- **`test_xattrs_acls.sh`**: Phase D — `user.test` xattr round-trip with
  `--xattrs --posix`, include/exclude filters, ACL + default-ACL round-trip
  with `--acls`, and extract without `--xattrs` leaves attrs unset. Skips
  when build or host lacks support.

- **`test_phase_e.sh`**: Phase E (G10–G15) — `--restrict` rejects `-P`,
  `--to-command`, multi-volume; `--backup=simple|none|numbered`;
  `--quoting-style=literal|escape|c|shell`; `--check-device` parse;
  listed-incremental snapshot V2 (dirs + device field); help text.

### Interoperability verification

Key interop checks in `run_tests.sh`:
- **T08**: `mutar -cf` then `tar -tf` — system tar must list all members
- **T09**: `tar -cf` then `mutar -xf` — mutar must extract system-tar archives
- **T17**: `mutar -tJf tar-1.35.tar.xz` — reads a real GNU tar 1.35 archive
- **T26/T27**: extract all 7 repo `.tar.*` archives, repack, re-extract,
  compare file counts — catches format-parsing regressions

---

## Phase E — Incremental / remote / polish (G10–G15) (2026-08-16)

| ID | Item | Status | Notes |
|----|------|--------|-------|
| G13 | `--restrict` | ✅ Implemented | Rejects `-P`/`--absolute-names`, `--to-command`, multi-volume (`-M`/`-L`/`-F`); clear error + non-zero exit |
| G15 | `--backup` CONTROL | ✅ Implemented | `none`/`off`, `simple`/`never` (suffix), `numbered`/`t` (`file.~N~`), `existing`/`nil` |
| G12 | `--quoting-style` | ✅ Implemented | `literal`, `escape`, `c`, `c-maybe`, `shell`, `shell-always` on `-t` and verbose extract |
| G14 | `--check-device` | ✅ Implemented | `Config::check_device` default true; snapshot V2 stores `st_dev`; re-archive when device changes |
| G10 | Incremental dirs | ⚠️ Partial | Snapshot records directories + specials; skip filter remains regular-file mtime (+dev) |
| G11 | rmt lseek | 📄 Documented only | Help + this file: rmt `S`/lseek and remote append not supported |

**Tests:** `tests/test_phase_e.sh` (CTest `mutar_phase_e_tests`).

**Snapshot format:** `MUTAR_SNAPSHOT_V2` lines are `name\tmtime\tdev`. V1 (`name\tmtime`) still readable.

# µtar (mutar) — TODO (next session)

**HEAD at last persist:** `82c5c0d` (2026-08-17)  
**Repo:** https://github.com/EdgeOfAssembly/mutar  
**Local:** `/tmp/mutar` (tmpfs — push before reboot)

---

## Done this long session (do not redo)

- Rename star → µtar/`mutar`; GitHub `EdgeOfAssembly/mutar`
- GOAL / GOAL_NEXT / GOAL_GNU_PARITY → **v0.3.0** (GNU tar 1.35 CLI except SELinux)
- Hunt R1–R3: zip-slip, crash/hang, leftover HIGH CLI, fuzz overflow
- **`make profile`**: linux src create **27.9s → 3.96s** (`write_regular` 64KiB + `write_bytes`); archive byte-identical
- Checkpoint: `checkpoint/20260817T004023Z-perf`

---

## Next session — leftover bugs (priority)

These are **real** GNU mismatches / polish, not crashes. Targeted FIXUP series, not another hunt round.

| Pri | Item | Notes |
|-----|------|--------|
| P1 | `--no-recursion` positional | GNU applies at the point it appears; mutar last-wins global |
| P1 | `-t`/`-x DIR` descendants | GNU lists/extracts dir contents when DIR is named; mutar may only match exact name |
| P1 | Delayed directory mode | Extracting into `0555` dirs can drop children; GNU uses safe_dir_mode then restore |
| P1 | `--occurrence` on `--delete` / `-d` | Flag ignored for those ops |
| P2 | `--newer-mtime=now`/`tomorrow` | Silently disables filter if date parse fails |
| P2 | `--suffix=` should imply `--backup`; honor `SIMPLE_BACKUP_SUFFIX` | |
| P2 | `--keep-directory-symlink` + `--delay-directory-restore` | Can chmod/utimens **outside** via followed link |
| P2 | `--xattrs-include` / `--xattrs-exclude` on **extract** | Filters apply on create only |
| P2 | `--transform` | ECMAScript vs GNU sed; symlink targets not rewritten |
| P3 | Sparse GNU **1.0 write/read** interop | mutar 1.0 map vs GNU `GNUSparseFile` / excess map |
| P3 | `--checkpoint-action=dot` | Per-member not per-record |
| P3 | `--one-file-system` | Drops xdev mount-point directory |
| P3 | `--utc` should imply verbose listing | |
| P3 | Numbered backup GNU max+1 vs first-gap | |

---

## Next session — performance (optional)

- List/extract still scan-bound (`read_block` / `block_checksum`)
- Re-run: `make -s -j$(nproc) profile` then gprof after `-tf` / `-xf`
- Compare wall time vs create 3.96s / list 1.58s on `/usr/src/linux`
- Keep archive **byte-identical** unless a format change is intentional
- Revert if slower (same loop as 2026-08-17)

---

## Experimental feature branch (not main)

**Idea:** entropy-aware member order — estimate Shannon entropy (or compressibility) per file, **sort create order by increasing entropy**, optionally **group** similar low-entropy (repetitive text) together.

**Hypothesis:** solid compressors (gzip/xz/zstd) get a slightly smaller `.tar.gz` / `.tar.xz` because similar / low-entropy streams sit in one window.

**Constraints:**
- New branch only, e.g. `exp/entropy-order` — **do not** change default GNU member order on `main`
- Opt-in flag, e.g. `--sort=entropy` (today `--sort=name|inode` only)
- Must not break `--preserve-order` / `-s` / incremental dumpdir semantics
- Measure: same `/usr/src/linux` tree, `mutar -cf - \| gzip` vs entropy-sorted, report size delta
- Cheap estimator first (byte histogram / 4k sample), not full compress twice

### Archive deltas (experimental — not GNU tar)

**Today:** mutar has **no** archive-to-archive delta/patch.  
`-g` / `-G` incremental is **filesystem** “which paths changed since snapshot”, not “diff two tars”.

**Idea:** given base archive A (plain or compressed) and new archive or tree B, emit a **delta** (plain or compressed) that can be applied onto A to reconstruct B. Chain: `1.0` base + `1.5` + `1.8` + `2.0` deltas.

**Why it might belong in mutar (not just `xdelta3 A B`):**
- Member-aware: match by name/hardlink, emit add / replace / delete / rename
- Reuse `MUTAR.INDEX.V1` + content hashes; only store changed members (or binary patches of those members)
- Apply without unpacking the whole tree to disk

**Why it is hard / easy to do badly:**
- Compressed A/B must be decompressed (or seekable) before a useful member delta
- Whole-file `xdelta`/`zstd --patch-from` already exists and often wins on two `.tar` blobs
- Apply-chain integrity needs checksums; a bad delta corrupts the whole product
- Not GNU tar; keep **off `main` default**

**Suggested branch:** `exp/archive-delta`  
Sketch CLI (names TBD): `--delta-from=BASE` on create; `--apply-delta=DELTA` onto a base. Measure size vs full B and vs `xdelta3` on the two tars.

**Mandatory if we ever test this:** checksum verification end-to-end — not optional.

1. Hash **base** and **new** trees/archives (e.g. sorted `find … -print0 | xargs -0 sha256sum`, plus archive SHA-256).
2. Apply delta(s) in order onto a copy of the base.
3. Hash the reconstructed result; **must match** the new tree/archive bit-for-bit (file count, sizes, contents).
4. Fail the experiment if any checksum mismatches — size wins do not count without this.
5. For a version chain (`1.0` + Δ`1.5` + Δ`1.8` + Δ`2.0`), verify after **each** apply, not only at the end.

---

## Still deferred (not scheduled)

| Item | Notes |
|------|--------|
| True compressed frame seek | No full materialize; liblzma / zstd seekable |
| Write GNU listed-incremental snapshot format 2 | Read works; write stays `MUTAR_SNAPSHOT_V2` |
| SELinux | Unsupported (no hardware) |
| Packaging (ebuild/deb) | Optional |
| Broader CBMC | Beyond `sanitize_path` |

## Policy

- SELinux: no-op + warning  
- CLI: 100% GNU tar 1.35 **switches** except SELinux  
- No “faster than GNU” without numbers  
- Dual-write durable facts: pmem + `~/.grok/memory/projects/mutar.md`

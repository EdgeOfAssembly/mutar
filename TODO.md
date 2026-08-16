# µtar (mutar) — TODO

## Done (see PROGRESS.md / GOAL_NEXT.md)

- v0.1.0 — rename, index, seekable compress, freeze  
- v0.2.0 — pax `delete=`, multi-vol between-member, xattrs/ACLs, Phase E polish, benches, formal path harness, `mutar.1`

## Active campaign

**GNU tar CLI 100% parity (except SELinux):** see **`GOAL_GNU_PARITY.md`** (target **v0.3.0**).  
That campaign **includes** mid-file multi-volume and full `--pax-option` (no longer deferred).

## Possible future work (still not in GOAL_GNU_PARITY)

| Item | Notes | Effort |
|------|--------|--------|
| **True compressed frame seek without materialize** | liblzma block index and/or zstd seekable format; keep materialize fallback. Today: selective extract of compressed archives decompresses fully once, then seeks. | High |

## Smaller / demand-driven (optional)

- More `--pax-option` keywords one-at-a-time when a real user needs them  
- `--mode` full GNU symbolic mode strings (`u+r`, `go-w`, …) — octal works  
- `-N`/`--newer` ctime semantics vs `--newer-mtime` (today both use mtime)  
- Multi-volume mid-file split for **sparse** members  
- Compressed remote rmt archives  
- GNU binary listed-incremental snapshot interop (mutar uses MUTAR_SNAPSHOT_V2)  
- Broader `--warning=KEYWORD` coverage (some sites still raw stderr)  
- Numbered backup edge cases matching GNU exactly  
- Packaging (ebuild / deb)  
- Broader CBMC coverage beyond `sanitize_path`  

**Done in GOAL_GNU_PARITY Phases 1–7 (do not re-list):** rmt `L` (lseek) + remote `-r`/`-u`, mid-file multi-vol, full `--pax-option`, preserve-order, exclude-ignore, dumpdir `-G`, listed-incremental skip, Phase 1 quick wins.

## Policy

- SELinux remains **unsupported** without test hardware.  
- Compatibility goal stays **~99%** GNU tar 1.35, not 100%.  
- No “faster than GNU” claims without published numbers (`PROGRESS.md` Phase F).

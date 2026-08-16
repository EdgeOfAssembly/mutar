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
- Numbered backup edge cases matching GNU exactly  
- Packaging (ebuild / deb)  
- Broader CBMC coverage beyond `sanitize_path`  
- Write GNU listed-incremental snapshot format (read of GNU format 2 is done; write remains MUTAR_SNAPSHOT_V2)

**Done in GOAL_GNU_PARITY Phases 1–8 residuals:** rmt `L` + remote `-r`/`-u` + **compressed remote**, mid-file multi-vol (**including sparse**), full `--pax-option`, preserve-order, exclude-ignore, dumpdir `-G`, listed-incremental skip + **GNU snapshot read**, `--mode` symbolic, `-N` ctime vs `--newer-mtime`, broader `--warning`.

## Policy

- SELinux remains **unsupported** without test hardware.  
- Compatibility goal stays **~99%** GNU tar 1.35, not 100%.  
- No “faster than GNU” claims without published numbers (`PROGRESS.md` Phase F).

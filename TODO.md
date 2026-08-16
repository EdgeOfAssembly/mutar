# µtar (mutar) — TODO

## Done (see PROGRESS.md)

- v0.1.0 — rename, index, seekable compress, freeze  
- v0.2.0 — pax `delete=`, multi-vol between-member, xattrs/ACLs, Phase E polish, benches, formal path harness, `mutar.1`  
- **v0.3.0** — `GOAL_GNU_PARITY` complete: full GNU tar 1.35 CLI except SELinux (mid-file multi-vol, full `--pax-option`, rmt, preserve-order, exclude-ignore, dumpdir, listed-incremental, `--ignore-failed-read`, `--recursive-unlink`, …)

## Possible future work

| Item | Notes | Effort |
|------|--------|--------|
| **True compressed frame seek without materialize** | liblzma block index and/or zstd seekable format; keep materialize fallback. Today: selective extract of compressed archives decompresses fully once, then seeks. | High |
| Write GNU listed-incremental snapshot format 2 | Read of GNU format 2 is done; write remains intentional **`MUTAR_SNAPSHOT_V2`** text | Medium |

## Smaller / demand-driven (optional)

- Numbered backup edge cases matching GNU exactly  
- Packaging (ebuild / deb)  
- Broader CBMC coverage beyond `sanitize_path`

## Policy

- SELinux remains **unsupported** without test hardware.  
- Compatibility: **YES** GNU tar 1.35 CLI except SELinux; listed-incremental write stays `MUTAR_SNAPSHOT_V2`.  
- No “faster than GNU” claims without published numbers (`PROGRESS.md` Phase F).

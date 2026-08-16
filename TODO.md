# µtar (mutar) — TODO

## Done (see PROGRESS.md / GOAL_NEXT.md)

- v0.1.0 — rename, index, seekable compress, freeze  
- v0.2.0 — pax `delete=`, multi-vol between-member, xattrs/ACLs, Phase E polish, benches, formal path harness, `mutar.1`

## Possible future work (not scheduled)

These were **explicitly deferred** after v0.2.0. Implement only with a focused goal and tests — not as silent scope creep.

| Item | Notes | Effort |
|------|--------|--------|
| **Mid-file multi-volume** | Split one large member across volumes (`GNUTYPE_MULTIVOL` / type `'M'`); reassemble on extract. Today: between-member rotation works; oversized single member errors clearly. | High |
| **Full `--pax-option` keyword set** | Beyond `delete=KEYWORD`: `exthdr.name`, `globexthdr.*`, time/uid/gid transforms, etc. Audit against GNU tar before claiming “full”. | Medium–high |
| **True compressed frame seek without materialize** | liblzma block index and/or zstd seekable format; keep materialize fallback. Today: selective extract of compressed archives decompresses fully once, then seeks. | High |

## Smaller / demand-driven (optional)

- More `--pax-option` keywords one-at-a-time when a real user needs them  
- rmt `S` (lseek) / remote append  
- Numbered backup edge cases matching GNU exactly  
- Packaging (ebuild / deb)  
- Broader CBMC coverage beyond `sanitize_path`

## Policy

- SELinux remains **unsupported** without test hardware.  
- Compatibility goal stays **~99%** GNU tar 1.35, not 100%.  
- No “faster than GNU” claims without published numbers (`PROGRESS.md` Phase F).

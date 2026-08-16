# µtar (mutar) — Progress / Proof Log

## Campaign freeze — 2026-08-16 (GOAL phases 0–7)

### Identity

| Item | Value |
|------|--------|
| Brand | **µtar** |
| Binary | **`mutar`** |
| Repo | https://github.com/EdgeOfAssembly/mutar |
| Not | Schily `star` |
| Compat goal | **~99%** GNU tar 1.35 |
| SELinux | **Unsupported** (policy; no test hardware) |

### Commits (main)

| SHA | Subject |
|-----|---------|
| `79f322b` | FEATURE v1 Rename star to µtar/mutar |
| `8636d8b` | FEATURE v1 Sidecar index MUTAR.INDEX.V1 and seek extract |
| `9387b26` | FEATURE v1 Seekable compression and materialize-then-seek |
| `c054b0a` | FIXUP v1 Document Phase 6 seekable compression |
| `380e38c` | FEATURE v1 Campaign freeze Phase 7 |

### Features delivered

1. **Rename** — product, namespace, CMake, tests, GitHub repo
2. **Sidecar index** — `MUTAR.INDEX.V1`, `--write-index`, `--mutar-index`
3. **Seek extract** — uncompressed `lseek`; compressed materialize-then-seek
4. **`--seekable`** — xz multi-block / zstd chunked write; implies index

### Verification (freeze gate)

```
ctest --test-dir build --output-on-failure
  5/5 tests passed (mutar_tests, pr172, index_seek, seekable_compress, boundary)

bash tests/test_index_seek.sh build/mutar          → PASS=14 FAIL=0
bash tests/test_seekable_compress.sh build/mutar   → PASS=11 FAIL=0
bash tests/test_sparse.sh build/mutar              → PASS=23 FAIL=0
bash tests/test_new_options.sh build/mutar         → PASS=22 FAIL=0
bash tests/test_formats_compression.sh build/mutar → PASS=79 FAIL=0
```

Sanitizer Debug build: `-fsanitize=address,undefined -fno-sanitize-recover=all`.

**formal:** not run (no CBMC properties for path traversal yet).

### Remaining gaps (honest)

- True frame-level seek without materialize (liblzma/libzstd)
- Full multi-volume mid-file stream swap
- xattrs/ACLs store/restore (flags partial)
- SELinux: never planned without hardware
- Some GNU tar options still partial/no-op (see COMPATIBILITY_PROGRESS.md)

### Tag

`v0.1.0` — first public freeze of µtar/mutar identity + index/seek.

---

## GOAL_NEXT phases A–D — 2026-08-16

**Status:** A–D complete; stop for user OK before E–H.  
**HEAD at proof:** see `git log -1` after FIXUP commit below.

### Delivered

| Phase | Scope | Result |
|-------|--------|--------|
| A | G5 honest tables + G22 path hygiene | `COMPATIBILITY_PROGRESS` / AGENTS / README aligned |
| B | G1 `--pax-option` | `delete=KEYWORD` on write; other keywords still ignored |
| C | G2–G4 multi-volume | Between-member create/extract, `-L`, `--volno-file`, `--info-script`; **mid-file split still partial** |
| D | G6–G8 xattrs/ACLs | SCHILY PAX store/restore + include/exclude; **SELinux unsupported (G9)** |

### Verification (A–D gate)

```
ctest --test-dir build --output-on-failure
  8/8 tests passed, 0 failed
  (mutar_tests, pr172, index_seek, seekable_compress, pax_option,
   multi_volume, xattrs_acls, boundary)

bash tests/run_tests.sh build/mutar                → PASS=41 FAIL=0
bash tests/test_formats_compression.sh build/mutar → PASS=79 FAIL=0
bash tests/test_sparse.sh build/mutar              → PASS=23 FAIL=0
bash tests/test_new_options.sh build/mutar         → PASS=22 FAIL=0
bash tests/test_index_seek.sh build/mutar          → PASS=14 FAIL=0
bash tests/test_seekable_compress.sh build/mutar   → PASS=11 FAIL=0
bash tests/test_pax_option.sh build/mutar          → 9 passed, 0 failed
bash tests/test_multi_volume.sh build/mutar        → 5 passed, 0 failed
bash tests/test_xattrs_acls.sh build/mutar         → 12 passed, 0 failed
```

Sanitizer Debug build: `-fsanitize=address,undefined -fno-sanitize-recover=all`.  
**formal:** not run (Phase G).

### Residual (do not claim done)

- Multi-volume mid-file stream split
- `--pax-option` keywords beyond `delete=`
- SELinux (out of scope)
- Phases E–H (incremental/remote, seek/perf, formal/man, v0.2.0 freeze)

# µtar (mutar) — GOAL_NEXT (post-v0.1.0)

**Status:** ready to execute  
**Date:** 2026-08-16  
**Owner:** EdgeOfAssembly  
**Local tree:** `/tmp/mutar`  
**Remote:** https://github.com/EdgeOfAssembly/mutar  
**Baseline:** tag `v0.1.0` (campaign `GOAL.md` phases 0–7 complete)

This file is the **single source of truth for new work** after the rename/index/seek freeze.
Do **not** re-run `GOAL.md` phases 0–7. Load this file first, then spawn subagents as specified.

---

## 0. Locked identity (do not reopen)

| Role | Value |
|------|--------|
| Brand | **µtar** |
| Binary / CLI | **`mutar`** |
| Namespace | `mutar` |
| Repo | `EdgeOfAssembly/mutar` |
| Local path | `/tmp/mutar` |
| Compat goal | **~99%** GNU tar 1.35 |
| SELinux | **Unsupported** (no hardware; never claim support) |
| License | GPL-3.0-or-later |

**Not Schily star.** Keep disclaimers in README / `--help` / `--version`.

---

## 1. North-star outcomes (what “done” means for this campaign)

When this campaign is complete:

1. **Fewer honest gaps** in `COMPATIBILITY_PROGRESS.md` (partial → implemented or explicitly “won’t do”).
2. **xattrs + ACLs** store/restore on create/extract when host libs present (SELinux stays out).
3. **Multi-volume** mid-file continuation works for create + extract (stream swap).
4. **`--pax-option`** applied when writing PAX headers.
5. **True compressed seek** path improved (optional libzstd/liblzma or documented materialize-only).
6. **Security formal** for path traversal / size validation (`make verify` or CBMC) **or** documented skip.
7. **Benchmarks published** (index list / seek extract vs sequential) without false “faster than GNU” claims unless measured.
8. All changes: sanitizer-clean tests, FEATURE/FIXUP commits, `git push origin`, durable gate on tmpfs.

---

## 2. Remaining gaps (backlog, prioritized)

### P0 — Correctness / honesty (do first)

| ID | Gap | Acceptance |
|----|-----|------------|
| G1 | `--pax-option` pure no-op (not stored) | Writing PAX archives honors delete/exthdr/name keywords needed for GNU tar interop tests |
| G2 | Multi-volume mid-file split incomplete | Create with `-M -L` can split a large file across volumes; extract reassembles |
| G3 | `--info-script` / `--new-volume-script` not exec’d | Script runs at volume boundary; non-zero exit handled |
| G4 | `--volno-file` no-op (field never assigned from CLI) | Volume number read/written atomically |
| G5 | Status tables still list some ✅ that are partial | Audit vs code; fix tables (Phase A honesty pass) |

### P1 — Extended attributes (no SELinux)

| ID | Gap | Acceptance |
|----|-----|------------|
| G6 | `--xattrs` store/restore | Create stores user.* (and filtered keys); extract restores; round-trip test |
| G7 | `--xattrs-include` / `--xattrs-exclude` | Filters applied on store |
| G8 | `--acls` store/restore | When `MUTAR_HAVE_ACL`; round-trip with `getfacl`/`setfacl` |
| G9 | SELinux | **Out of scope forever** unless hardware appears; keep no-op + warning |

### P2 — Incremental / remote / polish

| ID | Gap | Acceptance |
|----|-----|------------|
| G10 | Incremental only compares regular-file mtime | Dirs/symlinks/specials handled per GNU tar listed-incremental semantics (or document limits) |
| G11 | rmt `lseek` / remote append | Document or implement `S` command; append/update on remote |
| G12 | `--quoting-style` | List/verbose output respects style |
| G13 | `--restrict` | Dangerous options rejected when set |
| G14 | `--check-device` | Wired into incremental device checks |
| G15 | `--backup` / `--suffix` | Numbered/existing CONTROL (simple suffix rename already works) |

### P3 — Performance / seek evolution

| ID | Gap | Acceptance |
|----|-----|------------|
| G16 | Compressed seek always materializes full archive | Optional: xz block index or zstd seekable format without full temp when feasible |
| G17 | Benchmarks not in CI | `tests/bench_index_seek.sh` results recorded in `PROGRESS.md`; optional CTest `BENCHMARK` label (not fail gate) |
| G18 | Claim “faster than GNU/Schily” | Only after published numbers; otherwise never claim |

### P4 — Quality / packaging

| ID | Gap | Acceptance |
|----|-----|------------|
| G19 | No `make verify` / formal | CBMC or equivalent for `sanitize_path` + size/octal bounds; or `formal: not run` with reason |
| G20 | Man page is still GNU `tar.1` dump | Ship `mutar.1` describing µtar differences + Schily disclaimer |
| G21 | No installable package metadata | Optional: simple `mutar.spec` / ebuild notes; not required for code freeze |
| G22 | Workspace path hygiene | No live `/tmp/Star` references; agents use `/tmp/mutar` |

---

## 3. Campaign phases (strict order)

```
Phase A  Hygiene + audit (G5, G22) — tables honest; no stale paths
Phase B  --pax-option (G1)
Phase C  Multi-volume completion (G2–G4)
Phase D  xattrs + ACLs (G6–G8); SELinux stays unsupported (G9)
Phase E  Incremental/remote/polish (G10–G15) — pick by impact
Phase F  Seek/perf (G16–G18)
Phase G  Formal + man page (G19–G20)
Phase H  Freeze v0.2.0 — PROGRESS proof, tag, push, memory dual-write
```

**Hard rules:**
- Do not claim SELinux support.
- Do not break GNU tar interop on archives mutar creates.
- Sanitizer-clean tests required for every phase that changes behavior.
- Prefer small FEATURE/FIXUP series over giant commits.

---

## 4. Subagent topology

Orchestrator only (depth 1).

| Phase | Agents | Mode |
|-------|--------|------|
| A | 1× `[explore]` audit + 1× `[implement]` table/path fixes | read-only then all |
| B–D | 1× `[plan]` → 1× `[implement]` → 1× `[review]` → 1× `[test]` | serial |
| E | parallel `[implement]` only if non-overlapping files; else serial | worktree if parallel |
| F | 1× `[plan]` seek design → implement → bench | serial |
| G | 1× `[implement]` formal/man | all |
| H | orchestrator freeze | execute |

Handoffs: `$TMPDIR/grok-$(id -u)/mutar/`  
Prefixes: `[explore]`, `[plan]`, `[implement]`, `[review]`, `[test]`

---

## 5. Build & test (canonical)

```bash
cd /tmp/mutar

cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-std=c++23 -ggdb3 -O0 -gdwarf-5 \
    -fvar-tracking-assignments -fno-omit-frame-pointer \
    -fstack-protector-all \
    -fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

BIN=build/mutar
bash tests/run_tests.sh "$BIN"
bash tests/test_formats_compression.sh "$BIN"
bash tests/test_sparse.sh "$BIN"
bash tests/test_new_options.sh "$BIN"
bash tests/test_pr172_features.sh "$BIN"
bash tests/test_index_seek.sh "$BIN"
bash tests/test_seekable_compress.sh "$BIN"
# optional: BENCH_MEMBERS=2000 bash tests/bench_index_seek.sh "$BIN"
```

**Evidence rule:** no green claim without command + exit code 0.  
**formal:** run after green tests when touching path/size critical code (Phase G).

---

## 6. Git / durability

1. Checkpoint before FEATURE/FIXUP  
2. Subjects: `FEATURE vN …` / `FIXUP vN …` (series `X/Y` when needed)  
3. Push origin (tmpfs)  
4. `require-durable.sh --push` exit 0 before “done”  
5. Never force-push `main`

---

## 7. Orchestrator kickoff prompt (paste to start)

```
You are the orchestrator for µtar/mutar post-v0.1.0 work.

Read and obey /tmp/mutar/GOAL_NEXT.md end-to-end.
Do NOT re-run GOAL.md phases 0–7 (archived).

Locked: brand µtar, binary mutar, repo EdgeOfAssembly/mutar, tree /tmp/mutar.
~99% GNU tar 1.35; SELinux unsupported forever without hardware.

Order: Phase A → B → C → D → stop for user OK → E → F → G → H.

First actions:
1. cd /tmp/mutar; unset GITHUB_TOKEN; gh auth status; git status
2. Checkpoint git
3. Phase A: audit COMPATIBILITY_PROGRESS vs code; fix dishonest rows
4. Phase B: implement --pax-option with tests
5. Full sanitizer ctest + shell harnesses before declaring any phase done

Handoffs under $TMPDIR/grok-$(id -u)/mutar/.
Report after each phase: changes, commands+exit codes, residual risks.
```

---

## 8. Out of scope (this campaign)

- SELinux store/restore  
- Wayland / GUI  
- Windows-native port (optional later)  
- Replacing external compressors with linked libs unless Phase F needs it  
- Marketing “fastest tar” without benchmarks  
- Rewriting git history / force-push  

---

## 9. Success metrics (checklist)

- [x] Phase A: G22 path hygiene + G5 honest status tables (COMPAT/AGENTS/README)
- [x] G1 `--pax-option` tested (`delete=KEYWORD`)
- [ ] G2–G4 multi-volume mid-file + scripts + volno  
- [ ] G6–G8 xattrs/ACLs round-trip (when libs present)  
- [ ] SELinux still unsupported in help/docs  
- [ ] Sanitizer ctest + harnesses green  
- [ ] `PROGRESS.md` proof block for v0.2.0  
- [ ] Tag `v0.2.0` (or `v0.1.1` if only polish) pushed  
- [ ] Dual-write memory updated  

---

## 10. Relationship to other docs

| Doc | Role |
|------|------|
| `GOAL.md` | Archived rename/index/seek campaign (v0.1.0) |
| **`GOAL_NEXT.md`** | **This file — active backlog** |
| `PROGRESS.md` | Proof log (append each freeze) |
| `COMPATIBILITY_PROGRESS.md` | Option-by-option truth table |
| `ARCHITECTURE.md` | Design notes |
| `AGENTS.md` | Agent rules for this tree |

---

*Orchestrators: update this file when the user changes scope; do not invent a parallel plan.*

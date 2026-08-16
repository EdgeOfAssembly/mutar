# µtar (mutar) — GOAL: 100% GNU tar CLI (except SELinux)

**Status:** ready to execute  
**Date:** 2026-08-16  
**Owner:** EdgeOfAssembly  
**Local tree:** `/tmp/mutar`  
**Remote:** https://github.com/EdgeOfAssembly/mutar  
**Baseline:** tag `v0.2.0` + security FIXUPs on `main`  
**Parity audit:** `/tmp/grok-$(id -u)/mutar/cli-parity-report.md`  
**Help dumps:** `/tmp/tar.txt` (GNU tar 1.35), `/tmp/mutar.txt` (regenerate each phase)

This file is the **single source of truth** for closing every GNU tar command-line gap
except SELinux. Do **not** re-run `GOAL.md` / `GOAL_NEXT.md` campaigns.

---

## 0. Locked identity

| Role | Value |
|------|--------|
| Brand | **µtar** |
| Binary | **`mutar`** |
| Repo | `EdgeOfAssembly/mutar` |
| Local path | `/tmp/mutar` |
| **CLI goal** | **100% of GNU tar 1.35 switches accepted and fully implemented** |
| **SELinux** | **Out of scope forever** — keep `--selinux` / `--no-selinux` as no-op + warning |
| License | GPL-3.0-or-later |
| Build for QA | **Debug + ASan + UBSan** |

**Not Schily star.** Keep disclaimers.

**Definition of “fully implemented”:** observable behavior matches GNU tar 1.35 for that
option on Linux (create/extract/list as applicable), with a dedicated test that fails if
the option is a no-op. Help text must not claim more than code does.

**Definition of done for this campaign:**  
Re-run parity audit → **YES** (only SELinux remains policy-unsupported). Tag **v0.3.0**.

---

## 1. North-star outcomes

1. Every option in `tar --help` is either:
   - **Fully implemented** in mutar, or  
   - **SELinux-only** (policy no-op).
2. Zero “Accepted but no-op” / “Partial” rows for non-SELinux options in
   `COMPATIBILITY_PROGRESS.md`.
3. `--preserve` long option works (same as `-p` + `-s` once `-s` works).
4. Help (`mutar --help`) lists all implemented options honestly; no help-only ghosts.
5. Sanitizer-clean Debug tests; formal `make verify` still green.
6. Mutar extras kept: `--write-index`, `--mutar-index`, `--seekable`.
7. FEATURE/FIXUP series pushed; durable gate; dual-write memory at freeze.

---

## 2. Gap inventory (from cli-parity-report — work items)

### G0 — Not accepted

| ID | Option | Acceptance |
|----|--------|------------|
| G0.1 | `--show-snapshot-field-ranges` | Parse + implement (print snapshot field ranges like GNU tar) |

### G1 — Partial / no-op (must become full)

| ID | Option | Acceptance |
|----|--------|------------|
| G1.1 | `--pax-option` | Full GNU keyword set used by tar 1.35 (not only `delete=`): `delete`, `exthdr.name`, `globexthdr.name`, `exthdr.mtime`, uid/gid/uname/gname transforms, etc. Interop tests vs `tar --pax-option=…` |
| G1.2 | `--sparse-version` | Parse `MAJOR.MINOR`; emit matching sparse format on write; reject unsupported |
| G1.3 | `-s` / `--preserve-order` / `--same-order` | Honor member order constraints on extract when set (GNU semantics) |
| G1.4 | `-G` / `--incremental` | Old GNU incremental dumpdir create/extract behavior (or document equivalent + pass GNU-compatible dumpdir tests) |
| G1.5 | `-g` / `--listed-incremental` + `--level` | Skip filter for dirs/symlinks/specials per GNU; snapshot complete |
| G1.6 | `-M` / `--multi-volume` + mid-file `-L` | **Mid-file split** (`GNUTYPE_MULTIVOL` / type `M`); extract reassembly across volumes |
| G1.7 | `--rmt-command` / `--rsh-command` | rmt `S` (lseek) + remote append/update where GNU supports |
| G1.8 | `--unquote` / `--no-unquote` | Apply when reading names from `-T` / CLI per GNU |
| G1.9 | `--verbatim-files-from` / `--no-verbatim-files-from` | `-T` option handling vs verbatim names |
| G1.10 | `--ignore-command-error` / `--no-ignore-command-error` | Wire to `--to-command` child exit status |
| G1.11 | `--quote-chars` / `--no-quote-chars` | Real char-class quoting (not miswired to `--quoting-style`) |
| G1.12 | `--exclude-ignore` / `--exclude-ignore-recursive` | Per-directory ignore-file semantics (GNU) |
| G1.13 | `--atime-preserve[=METHOD]` | Honor `replace` / `system` methods |
| G1.14 | `--totals[=SIGNAL]` | Optional SIGNAL: print totals on signal |
| G1.15 | `-o` | Create: force old V7 format like GNU; extract: keep no-same-owner |
| G1.16 | `--quoting-style` | Add `shell-escape`, `shell-escape-always`, `locale`, `clocale` |
| G1.17 | `--preserve` | Add to `long_opts` = `-p` + `-s` (after G1.3) |

### G2 — SELinux (do not “implement”)

| ID | Option | Required behavior |
|----|--------|-------------------|
| G2.1 | `--selinux` / `--no-selinux` | Keep no-op + clear warning; never claim support; never store/restore contexts |

### G3 — Audit hygiene (continuous)

| ID | Task |
|----|------|
| G3.1 | After each phase: regenerate `/tmp/tar.txt` / `/tmp/mutar.txt`; update parity report |
| G3.2 | No option in help that is not in `long_opts` |
| G3.3 | COMPATIBILITY_PROGRESS status table matches code |

---

## 3. Campaign phases (multi-agent)

```
Phase 0   Orient + checkpoint + refresh help dumps + gap matrix freeze
Phase 1   Quick wins (parse-only / small wire-ups): G0.1, G1.8–G1.11, G1.13–G1.17, G1.2
Phase 2   Files-from / exclude-ignore: G1.9, G1.12
Phase 3   Incremental: G1.4, G1.5
Phase 4   Full --pax-option: G1.1
Phase 5   Mid-file multi-volume: G1.6
Phase 6   rmt lseek + remote append: G1.7
Phase 7   Preserve-order extract: G1.3 (+ G1.17 if not done)
Phase 8   Parity re-audit + honesty docs + help sync
Phase 9   Full Debug sanitizer matrix + formal verify
Phase 10  Freeze v0.3.0 — PROGRESS, tag, push, memory dual-write
```

**Hard rules:**
- SELinux stays unsupported (G2).
- Debug + ASan + UBSan for all verification.
- One logical FEATURE/FIXUP series per phase (or X/Y sub-series if large).
- Prefer worktree isolation when two implementers touch `mutar.cpp` in parallel.
- Bugs found in QA: fix before next phase.
- Do not claim 100% until Phase 8 audit says **YES**.

---

## 4. Subagent topology

Orchestrator (you) only spawns. Depth = 1.

| Phase | Agents | Isolation |
|-------|--------|-----------|
| 0 | 1× `[explore]` inventory | read-only |
| 1 | 2× `[implement]` in **worktrees** (split G0/G1 quick wins) → 1× `[review]` | worktree then merge by orchestrator |
| 2–7 | 1× `[plan]` (optional for 4–6) → 1× `[implement]` → 1× `[test]` → 1× `[review]` | serial; worktree for implement |
| 8 | 1× `[explore]` parity re-audit + 1× `[implement]` docs | parallel explore/docs |
| 9 | 3× `[test]` worktrees (core / feature / formal) + 1× `[review]` | worktree each |
| 10 | orchestrator freeze | main tree |

**Description prefixes:** `[explore]`, `[plan]`, `[implement]`, `[review]`, `[test]`

**Handoffs:** `$TMPDIR/grok-$(id -u)/mutar/parity/`  
e.g. `phase1-summary.md`, `phase5-mvol-review.md`, `phase8-parity-YES.md`

**Merge rule:** only orchestrator (or single implementer on main) merges worktrees; never two writers on main `mutar.cpp` without serializing.

---

## 5. Phase prompts (copy into spawn)

### Phase 0 — Orient

```
[explore] cd /tmp/mutar. Refresh:
  tar --help > /tmp/tar.txt
  build/mutar --help > /tmp/mutar.txt
Diff against GOAL_GNU_PARITY.md §2. Confirm each G-id still open in code.
Write $TMPDIR/grok-$(id -u)/mutar/parity/phase0-matrix.md
Checkpoint: git branch checkpoint/UTC-gnu-parity
```

### Phase 1 — Quick wins (parallel worktrees)

**Worker A:** G1.8 unquote, G1.10 ignore-command-error, G1.11 quote-chars, G1.13 atime-preserve METHOD, G1.14 totals SIGNAL, G1.15 `-o` create v7, G1.16 quoting styles, G1.17 --preserve (depends on -s stub warning removal only if -s still no-op — implement flag storage; full -s in Phase 7)

**Worker B:** G0.1 show-snapshot-field-ranges, G1.2 sparse-version, G1.9 verbatim-files-from

Each: tests + Debug sanitizer + commit on worktree branch; orchestrator merges.

### Phase 2 — exclude-ignore + files-from

```
[implement] G1.9 (if not done) + G1.12 --exclude-ignore{,-recursive}
GNU per-directory ignore files. Tests with nested dirs.
Commit FEATURE v1 exclude-ignore and files-from parity
```

### Phase 3 — Incremental

```
[implement] G1.4 -G incremental dumpdir + G1.5 listed-incremental full skip semantics
Compare with GNU tar -g/-G fixtures. Sanitizer tests.
```

### Phase 4 — Full pax-option

```
[plan] then [implement] G1.1 full --pax-option
Reference: GNU tar pax.c option parsing. Interop: tar create with options, mutar read; mutar create, tar read.
```

### Phase 5 — Mid-file multi-volume

```
[plan] then [implement] G1.6 GNUTYPE_MULTIVOL mid-file split/reassemble
Large single file > tape_length. Extract across vol1+vol2. GNU tar interop both directions if possible.
```

### Phase 6 — rmt

```
[implement] G1.7 rmt S/lseek + remote append
May need mock rmt or documented integration test with ssh loopback if available; else unit-level protocol tests.
```

### Phase 7 — preserve-order

```
[implement] G1.3 -s/--preserve-order + ensure --preserve long opt
```

### Phase 8 — Parity re-audit

```
[explore] Produce new cli-parity-report. Answer must be YES except SELinux.
[implement] Fix any residual gaps or dishonest docs. Help sync.
```

### Phase 9 — Multiagent QA

```
Worktrees: qa-core, qa-feat, qa-formal, qa-review (Debug ASan+UBSan).
Same pattern as post-v0.2.0 multiagent QA. Fix bugs as they appear.
```

### Phase 10 — Freeze v0.3.0

```
Version bump 0.3.0, PROGRESS.md, GOAL_GNU_PARITY checklist all [x],
tag v0.3.0, gh release, pmem dual-write, require-durable --push
Update TODO.md: remove items completed by this campaign; keep true-compressed-seek if still deferred
```

---

## 6. Build & test (canonical)

```bash
cd /tmp/mutar

cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-std=c++23 -ggdb3 -O0 -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
make test && make verify

BIN=build/mutar
# full shell matrix + new parity tests added per phase
bash tests/run_tests.sh "$BIN"
bash tests/test_formats_compression.sh "$BIN"
bash tests/test_sparse.sh "$BIN"
bash tests/test_new_options.sh "$BIN"
bash tests/test_index_seek.sh "$BIN"
bash tests/test_seekable_compress.sh "$BIN"
bash tests/test_pax_option.sh "$BIN"
bash tests/test_multi_volume.sh "$BIN"
bash tests/test_xattrs_acls.sh "$BIN"
bash tests/test_phase_e.sh "$BIN"
bash tests/test_extract_safety.sh "$BIN"
# plus phase-specific tests

# Parity gate
tar --help > /tmp/tar.txt
"$BIN" --help > /tmp/mutar.txt
# explore agent must produce YES
```

**Evidence:** command + exit 0. No green without proof.  
**formal:** `make verify` required at Phase 9–10; run after path/security-sensitive changes.

---

## 7. Git / durability

1. Checkpoint before campaign and before Phases 4–6  
2. `FEATURE vN …` / `FIXUP vN …` / series `X/Y`  
3. Push origin after each phase  
4. `require-durable.sh --push` exit 0 before “done”  
5. Never force-push `main`

---

## 8. Orchestrator kickoff prompt (paste to start)

```
You are the orchestrator for µtar/mutar GNU tar CLI 100% parity (except SELinux).

Read and obey /tmp/mutar/GOAL_GNU_PARITY.md end-to-end.
Do NOT re-run GOAL.md or GOAL_NEXT.md.

Locked: brand µtar, binary mutar, tree /tmp/mutar, repo EdgeOfAssembly/mutar.
SELinux stays no-op+warning. Everything else from GNU tar 1.35 CLI must be fully implemented.

Order: Phase 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10.
Use multiple subagents per topology; worktree isolation when parallel writers.
Debug ASan+UBSan only for verification. Fix bugs as they appear.

First actions:
1. cd /tmp/mutar; unset GITHUB_TOKEN; git status; checkpoint
2. Spawn [explore] Phase 0 matrix
3. Phase 1 parallel worktrees for quick wins
4. Continue phases; after Phase 8 parity must answer YES
5. Phase 9 multiagent QA; Phase 10 tag v0.3.0

Handoffs: $TMPDIR/grok-$(id -u)/mutar/parity/
Report after each phase: commits, tests+exit codes, residual gaps.
```

---

## 9. Out of scope

- SELinux store/restore  
- True compressed frame seek without materialize (still `TODO.md` unless pulled in)  
- Windows-native port  
- Claiming faster-than-GNU without benches  
- Schily star compatibility  

---

## 10. Success metrics (checklist)

- [ ] G0.1 `--show-snapshot-field-ranges`  
- [x] G1.1 full `--pax-option`  
- [ ] G1.2 `--sparse-version`  
- [x] G1.3 `-s` / preserve-order  
- [x] G1.4 `-G` incremental  
- [x] G1.5 `-g` listed-incremental complete  
- [x] G1.6 mid-file multi-volume  
- [x] G1.7 rmt lseek + remote append  
- [ ] G1.8–G1.11 unquote, verbatim, ignore-command-error, quote-chars  
- [x] G1.12 exclude-ignore*  
- [ ] G1.13–G1.16 atime-preserve, totals SIGNAL, `-o` create, quoting styles  
- [ ] G1.17 `--preserve` long option  
- [ ] G2 SELinux still policy no-op only  
- [ ] Phase 8 parity report = **YES** (except SELinux)  
- [ ] Phase 9 multiagent Debug QA SHIP  
- [ ] Tag **v0.3.0** pushed + durable  

---

## 11. Related docs

| Doc | Role |
|------|------|
| **`GOAL_GNU_PARITY.md`** | **This campaign (active)** |
| `GOAL_NEXT.md` | Archived post-v0.1.0 (complete at v0.2.0) |
| `GOAL.md` | Archived rename campaign |
| `TODO.md` | Future items; update at Phase 10 |
| `COMPATIBILITY_PROGRESS.md` | Live status table |
| `cli-parity-report.md` | Baseline audit (regenerate Phase 8) |

---

*Orchestrators: update this file when the user changes scope; do not invent a parallel plan.*

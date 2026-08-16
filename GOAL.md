# µtar (mutar) — Master Goal Prompt

**Status:** phases 0–7 complete (2026-08-16)  
**Date:** 2026-08-16  
**Owner:** EdgeOfAssembly  
**Local tree:** `/tmp/Star`  
**Current remote:** `https://github.com/EdgeOfAssembly/star`  
**Target remote:** `https://github.com/EdgeOfAssembly/mutar`

This file is the **single source of truth** for the multi-agent campaign.
Any orchestrator session should load this first, then spawn subagents as specified.

---

## 0. Identity (locked decisions)

| Role | Value |
|------|--------|
| **Brand / display name** | **µtar** (spoken: “mu-tar”) |
| **Binary / package / CLI** | **`mutar`** (ASCII only) |
| **GitHub repo** | **`EdgeOfAssembly/mutar`** (rename in place; do not create a new empty repo) |
| **C++ namespace** | `mutar` (was `star`) |
| **Man page** | `mutar(1)` |
| **Snapshot magic** | `MUTAR_SNAPSHOT_V1` (was `MUTAR_SNAPSHOT_V1`) |
| **Index format (future)** | `MUTAR.INDEX.V1` / sidecar `*.mutaridx` |
| **License** | GPL-3.0-or-later (unchanged) |
| **Language** | C++23, gcc/g++, CMake ≥ 3.20 |
| **Compatibility target** | **~99% GNU tar 1.35** (see §1) |

**Not Schily star.** README and `--help` must say explicitly:

> µtar (`mutar`) is a modern C++23 GNU-tar-compatible archiver.  
> It is **not** Jörg Schilling’s `star` (Schily tools).

---

## 1. Compatibility policy (99%, not 100%)

### In scope (must work / aim for parity)

- Archive formats: v7, oldgnu, gnu, ustar, pax/posix — read + write
- Main ops: create, extract, list, append, update, delete, diff, catenate, test-label
- Compression via external filters: gzip, bzip2, xz, zstd, lzma, lzip, lzop, compress, custom
- Sparse files, hard links, long names, PAX extended headers
- Path/exclude/transform/owner/mtime/mode options that GNU tar documents
- Interop: GNU tar reads mutar archives; mutar reads GNU tar archives
- Optional features when host libs exist: **xattrs**, **POSIX ACLs**

### Explicitly out of scope (document as “not supported”)

| Feature | Reason | Required behavior |
|---------|--------|-------------------|
| **SELinux** (`--selinux`, `--no-selinux`) | No SELinux machine available; cannot develop or test | **Do not claim support.** Prefer: omit from default help, or accept and print a clear “not supported” diagnostic. Never store/restore SELinux contexts. |
| Full multi-volume mid-file split | Hard; partial prompts exist | Honest partial status until implemented |
| Full rmt lseek / remote append | Partial bridge only | Document limits |
| 100% of every GNU tar edge case | Practical limit | Track gaps in `COMPATIBILITY_PROGRESS.md` |

**Wording for docs/help:**

> Goal: **~99% GNU tar 1.35 compatibility**.  
> SELinux context support is **not implemented and not planned** (no test hardware).  
> xattrs/ACLs may be partial depending on build-time detection.

Do **not** say “100% compatible” in README, help, or marketing.

---

## 2. End-state vision (definition of done for the whole campaign)

When the campaign is complete, all of the following are true:

1. **Identity**
   - Repo is `EdgeOfAssembly/mutar`; old `…/star` URL redirects.
   - Binary is `mutar`; `mutar --help` / `mutar -v` / no-args → usage.
   - No user-facing “star” product name remains (except historical changelog notes and “not Schily star” disclaimers).
   - CMake project, targets, tests, scripts, docs, AGENTS.md all say mutar/µtar.

2. **Quality bar**
   - Debug build with ASan+UBSan clean.
   - Full test suites green (`ctest`, shell harnesses).
   - `make test` (or CMake/ctest equivalent documented) exit 0.
   - Honest status tables: ✅ / ⚠️ / ❌ — no no-ops claimed as done.

3. **Compatibility**
   - Documented ~99% policy; SELinux explicitly unsupported.
   - Interop tests with system `tar` still pass.
   - Remaining gaps listed in one place (`COMPATIBILITY_PROGRESS.md`).

4. **Advanced features (index + seek)** — after rename
   - Optional sidecar index `MUTAR.INDEX.V1` / `*.mutaridx`.
   - Fast list / single-member extract when index present.
   - Seek on seekable fds; solid gzip remains sequential (documented).
   - Archives remain GNU-tar-readable (index is sidecar or ignorable member).

5. **Git durability**
   - Checkpoint before work; FEATURE/FIXUP commits; `git push origin` (tmpfs-safe).
   - No multi-hour dirty tree on `/tmp` without commit+push.

---

## 3. Campaign phases (strict order)

```
Phase 0  Auth + orient + checkpoint
Phase 1  GitHub rename (star → mutar)
Phase 2  Tree-wide product rename (star → mutar / µtar)
Phase 3  CLI sane defaults + help/version polish
Phase 4  Compatibility honesty pass (SELinux out; status tables)
Phase 5  Index + seek (sidecar first)
Phase 6  Seekable compression (zstd/xz) + benchmarks
Phase 7  Docs freeze + release notes + durable memory
```

**Hard rule:** Do not start Phase 5 until Phases 1–4 are merged/pushed and tests are green.

---

## 4. Subagent topology

Orchestrator (main session) only spawns. Depth = 1. No child-of-child.

| Phase | Subagents | Mode | Isolation |
|-------|-----------|------|-----------|
| 0 | 1× `[explore]` inventory | read-only | none |
| 1 | Orchestrator **or** 1× `[implement]` gh rename | execute | none |
| 2 | 1× `[implement]` rename tree; then 1× `[review]` | all / read-only | none (single writer) |
| 3 | 1× `[implement]` CLI polish | all | none |
| 4 | 1× `[explore]` gap audit ∥ 1× `[implement]` docs/code honesty | parallel | worktree for implement if needed |
| 5 | 1× `[plan]` index design → 1× `[implement]` → 1× `[review]` → 1× `[test]` | serial | none |
| 6 | 1× `[implement]` seekable compress; 1× `[test]` benches | serial | none |
| 7 | 1× `[review]` final; orchestrator push + memory | read-only + orchestrator | none |

**Description prefixes (required):** `[explore]`, `[plan]`, `[implement]`, `[review]`, `[test]`

**Handoffs:** write summaries under  
`$TMPDIR/grok-$(id -u)/mutar/`  
e.g. `phase2-rename-summary.md`, `phase5-index-review.md`  
Parent keeps paths + decisions, not full file dumps.

---

## 5. Phase-by-phase prompts (copy into spawn)

### Phase 0 — Orient

**`[explore]` inventory**

```
Thoroughness: medium
Repo: /tmp/Star (soon mutar)
Return a bullet inventory:
- All paths that contain product name "star" / "Star" / namespace mutar::
- Build: CMake targets, binary name, ctest names
- Test entry points and how to run them
- SELinux / xattr / ACL ifdefs and help text locations
- Git remote, branch, dirty state
Do not edit. Prefer rg. Skip build/ and .git objects.
Write: $TMPDIR/grok-$(id -u)/mutar/phase0-inventory.md
```

**Orchestrator:**  
`unset GITHUB_TOKEN`; `gh auth status`; confirm `EdgeOfAssembly` can rename `star`.  
Checkpoint: `git branch checkpoint/$(date -u +%Y%m%dT%H%M%SZ)-pre-mutar`

---

### Phase 1 — GitHub rename

**Goal:** `EdgeOfAssembly/star` → `EdgeOfAssembly/mutar` in place (preserve issues/stars/history).

```bash
unset GITHUB_TOKEN
gh repo rename mutar --repo EdgeOfAssembly/star
# verify:
gh repo view EdgeOfAssembly/mutar --json name,url
git -C /tmp/Star remote set-url origin git@github.com:EdgeOfAssembly/mutar.git
# or https://github.com/EdgeOfAssembly/mutar.git
git ls-remote origin HEAD
```

**Acceptance:**
- `gh repo view EdgeOfAssembly/mutar` works
- Old URL redirects (optional check: `curl -sI https://github.com/EdgeOfAssembly/star | head`)
- Local `origin` points at mutar
- **No** new empty repo; **no** force-push; **no** history rewrite

**Commit:** none required for GitHub-side rename; local remote URL change only.

---

### Phase 2 — Tree-wide rename

**`[implement]` product rename**

```
Task: Rename product star → µtar/mutar across /tmp/Star.

LOCKED:
- Brand: µtar
- Binary/CLI: mutar
- Namespace: mutar
- Snapshot: MUTAR_SNAPSHOT_V1
- CMake project(mutar …); target mutar
- Tests invoke build/mutar
- Help: "GNU tar-compatible archiver (µtar)"; disclaimer not Schily star
- Keep GPL-3.0-or-later

DO:
- star.cpp/hpp → mutar.cpp/hpp (or keep filenames if simpler BUT binary must be mutar)
- Replace namespace mutar:: → mutar::
- MUTAR_HAVE_* macros → MUTAR_HAVE_* (or keep MUTAR_HAVE_ for less churn — prefer MUTAR_HAVE_*)
- Update README, ARCHITECTURE, COMPATIBILITY_PROGRESS, AGENTS.md, tar.md references
- Update all tests/*.sh and CMakeLists.txt
- print_usage / --version strings
- Do NOT implement index/seek yet
- Do NOT claim SELinux support

Verify:
  cmake -B build -DCMAKE_BUILD_TYPE=Debug …
  cmake --build build -j$(nproc)
  ctest --test-dir build --output-on-failure
  bash tests/run_tests.sh build/mutar
  # other harnesses if time

Write summary: $TMPDIR/grok-$(id -u)/mutar/phase2-rename-summary.md
Commit: FEATURE v1 Rename star to µtar/mutar
Push origin after green.
```

**`[review]` rename**

```
Scope: phase2 diff vs checkpoint
Check: leftover user-facing "star" product name; binary name; broken test paths;
       SELinux not claimed; Schily disclaimer present; no accidental license change.
Write: $TMPDIR/grok-$(id -u)/mutar/phase2-review.md
Do not edit source.
```

---

### Phase 3 — CLI sane defaults

**`[implement]` CLI polish** (skill: cli-design)

```
Binary mutar must:
- No args → usage (same as -h/--help) on stderr or stdout consistently; exit non-zero or 0 per project choice (document it; prefer usage + exit 0 for --help, exit 2 or 1 for no-args if no operation — match GNU tar if reasonable)
- -h / --help
- -v / --version from 0.1 or 1.0.0 (bump on feature; NEVER -v = verbose — verbose stays GNU-style or use --verbose only; if -v is already verbose for tar compat, document conflict with cli-design and prefer GNU tar compatibility: -v verbose, --version for version)
IMPORTANT: GNU tar uses -v for verbose and --version for version. For 99% tar compat, KEEP -v = verbose and --version for version. Do not break tar muscle memory.
- Help groups like tar.1
- No-ops marked "(not yet implemented)" or omitted
- SELinux: not in help as supported; if parsed for compat, say not supported

Verify: mutar --help; mutar --version; mutar (no args); existing tests green
Commit: FEATURE v1 CLI help and version polish for mutar
```

---

### Phase 4 — Compatibility honesty (SELinux + status)

**`[explore]` gap audit**

```
Audit mutar vs tar.1 / COMPATIBILITY_PROGRESS.md.
Produce table: option → Implemented | Partial | No-op | Unsupported.
Force SELinux row = Unsupported (by policy).
Write: $TMPDIR/grok-$(id -u)/mutar/phase4-gap-audit.md
```

**`[implement]` honesty pass**

```
Apply audit:
- README + COMPATIBILITY_PROGRESS + ARCHITECTURE: ~99% goal; SELinux unsupported
- Remove or #ifdef-out SELinux store/restore claims; options may remain as rejected/not supported
- Fix any false ✅ for no-ops
- Keep xattrs/ACLs as optional/partial if code matches
Verify tests still green
Commit: FIXUP v1 Honest 99% compatibility; SELinux unsupported
```

---

### Phase 5 — Index + seek (sidecar first)

Reference: `/tmp/star-advanced-upgrade/STAR_ADVANCED_UPGRADE_INDEXING_SEEKING.md`  
(adapt all STAR.* names to MUTAR.*)

**`[plan]`**

```
Design ArchiveIndex + IndexEntry for mutar.
- Sidecar *.mutaridx, format MUTAR.INDEX.V1
- Zero cost when unused
- GNU tar still reads the .tar
- seek_to on seekable ArchiveStream
- CLI: --mutar-index / --write-index / explicit path (avoid clashing with GNU --index-file)
- Test plan: tests/test_index_seek.sh
- Out of scope: solid gzip random seek
Write: $TMPDIR/grok-$(id -u)/mutar/phase5-plan.md
```

**`[implement]` → `[review]` → `[test]`** serial after plan approval by orchestrator.

**Acceptance:**
- Create archive + index; list via index faster path works
- Extract one middle file uses lseek when seekable (strace or instrumentation)
- Without index: identical sequential behavior
- GNU tar -tf archive still works
- Sanitizer-clean tests

---

### Phase 6 — Seekable compression

- Prefer zstd seekable frames / xz block index when `--seekable`
- Document gzip solid = sequential only
- Micro-benchmarks: list 10k members; extract one file from large archive
- Commit FEATURE series; push

---

### Phase 7 — Freeze + memory

- Final `[review]` of whole tree vs GOAL.md
- Update README feature table + PROGRESS proof block
- Dual-write durable facts: pmem + `~/.grok/memory/projects/mutar.md`
- Tag optional `v0.1.0` or `v1.0.0-alpha` only if tests green
- `require-durable.sh --push` (or equivalent push proof)

---

## 6. Build & test commands (canonical)

```bash
cd /tmp/Star   # or renamed workdir

# Tools (once)
# gzip bzip2 xz zstd lzma lzip lzop gdb cmake build-essential

# Debug + sanitizers (mandatory before claiming green)
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-std=c++23 -ggdb3 -O0 -gdwarf-5 \
    -fvar-tracking-assignments -fno-omit-frame-pointer \
    -fstack-protector-all \
    -fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

# Shell harnesses (binary name after Phase 2)
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

**Evidence rule:** no “green” claim without command + exit code 0 in the report.

**Formal:** `formal: not run` unless CBMC properties are added for path traversal / size validation; not required for rename phases.

---

## 7. Git rules (every phase that edits)

1. Orient: `git status`, `git diff`, `git log -5 --oneline`
2. Checkpoint branch/tag before FEATURE/FIXUP
3. Commit subjects:
   - `FEATURE v1 Rename star to µtar/mutar`
   - `FEATURE v1 Sidecar index MUTAR.INDEX.V1`
   - `FIXUP v1 Honest SELinux unsupported`
4. Push: `git push -u origin HEAD` (tmpfs durability)
5. Never force-push `main` / shared history

---

## 8. Quality injection (every implement/review prompt)

- **C++23:** modern-c-cpp-quality — Allman braces, RAII, `std::expected`, Doxygen on public APIs, no raw owning pointers without cause, gcc only
- **CLI:** cli-design **except** where GNU tar flag semantics win (`-v` = verbose)
- **Tests:** max-quality-testing — add/adjust tests when behavior changes
- **Security:** path traversal checks on extract; `mkdtemp`; validate sizes before alloc
- **Honesty:** tar.1 is ground truth; no-op ≠ implemented

---

## 9. Orchestrator kickoff prompt (paste to start a session)

```
You are the orchestrator for the µtar/mutar campaign.

Read and obey /tmp/Star/GOAL.md end-to-end.

Locked identity: brand µtar, binary mutar, repo EdgeOfAssembly/mutar.
Compatibility: ~99% GNU tar 1.35; SELinux unsupported (no hardware).
Order: Phase 0 → 1 → 2 → 3 → 4 → stop for user OK → 5 → 6 → 7.

First actions:
1. unset GITHUB_TOKEN; gh auth status; confirm EdgeOfAssembly/star access
2. Spawn [explore] phase0 inventory
3. Checkpoint git
4. Phase 1 gh repo rename star → mutar
5. Phase 2 tree rename with implement + review
6. Run full sanitizer test suite yourself before declaring any phase done

Use subagents per GOAL.md topology. Handoffs under $TMPDIR/grok-$(id -u)/mutar/.
Do not start index/seek until rename + honesty phases are green and pushed.
Report after each phase: what changed, commands+exit codes, residual risks.
```

---

## 10. Out of scope for this campaign

- Wayland / GUI
- Windows-native port (optional later via mingw)
- Replacing external compressors with linked libzstd (optional later)
- Claiming faster-than-GNU without published benchmarks (Phase 6 may add numbers)
- Supporting SELinux “someday” without hardware — do not leave stubs that claim support

---

## 11. Success metrics (checklist)

- [x] `https://github.com/EdgeOfAssembly/mutar` exists (renamed)
- [x] `mutar --version` and `mutar --help` work; Schily disclaimer present
- [x] Zero product-name confusion with Schily `star` in README title
- [x] ASan+UBSan ctest + shell suites exit 0
- [x] SELinux documented as unsupported; not claimed in feature tables
- [x] Compatibility language is “~99%”, not “100%”
- [x] Sidecar index + seek path tested (Phases 5–6)
- [x] All FEATURE/FIXUP commits pushed to origin

---

*End of GOAL.md — orchestrators should not invent parallel plans; update this file if the user changes scope.*

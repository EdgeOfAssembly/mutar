#!/usr/bin/env bash
# tests/test_xattrs_acls.sh — xattrs + ACLs store/restore via SCHILY PAX (Phase D / G6–G8)
#
# Skips cleanly when:
#   - mutar was built without MUTAR_HAVE_XATTR / MUTAR_HAVE_ACL
#   - setfattr/getfattr (or setfacl/getfacl) are missing
#   - filesystem does not support user xattrs / ACLs
#
# SELinux is intentionally not tested (unsupported by project policy).
#
# Usage:
#   bash test_xattrs_acls.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
PASS=0
FAIL=0
SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1 — $2"; SKIP=$((SKIP + 1)); }

if [ ! -x "$MUTAR" ]; then
  echo "mutar binary not found/executable: $MUTAR" >&2
  exit 1
fi

HELP="$("$MUTAR" --help 2>&1 || true)"
HAS_XATTR_OPT=0
HAS_ACL_OPT=0
echo "$HELP" | grep -qF -- '--xattrs' && HAS_XATTR_OPT=1
echo "$HELP" | grep -qF -- '--acls' && HAS_ACL_OPT=1

WORK="$(mktemp -d /tmp/mutar_xattrs_acls.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

SRC="$WORK/src"
OUT="$WORK/out"
mkdir -p "$SRC" "$OUT"

# ── Probe host tools + filesystem ─────────────────────────────────────────────
HAVE_SETFATTR=0
HAVE_GETFATTR=0
HAVE_SETFACL=0
HAVE_GETFACL=0
command -v setfattr >/dev/null 2>&1 && HAVE_SETFATTR=1
command -v getfattr >/dev/null 2>&1 && HAVE_GETFATTR=1
command -v setfacl  >/dev/null 2>&1 && HAVE_SETFACL=1
command -v getfacl  >/dev/null 2>&1 && HAVE_GETFACL=1

FS_XATTR=0
FS_ACL=0
if [ "$HAVE_SETFATTR" -eq 1 ] && [ "$HAVE_GETFATTR" -eq 1 ]; then
  printf 'probe\n' >"$WORK/xattr_probe"
  if setfattr -n user.mutar_probe -v ok "$WORK/xattr_probe" 2>/dev/null \
     && getfattr -n user.mutar_probe --only-values "$WORK/xattr_probe" 2>/dev/null | grep -qx 'ok'; then
    FS_XATTR=1
  fi
fi
if [ "$HAVE_SETFACL" -eq 1 ] && [ "$HAVE_GETFACL" -eq 1 ]; then
  printf 'probe\n' >"$WORK/acl_probe"
  if setfacl -m u:nobody:r "$WORK/acl_probe" 2>/dev/null \
     && getfacl -c --absolute-names "$WORK/acl_probe" 2>/dev/null | grep -q 'user:nobody:r'; then
    FS_ACL=1
  fi
fi

echo "=== mutar xattrs/ACLs (Phase D) ==="
echo "  build: --xattrs=$HAS_XATTR_OPT --acls=$HAS_ACL_OPT"
echo "  host:  setfattr=$HAVE_SETFATTR getfattr=$HAVE_GETFATTR setfacl=$HAVE_SETFACL getfacl=$HAVE_GETFACL"
echo "  fs:    user_xattr=$FS_XATTR acl=$FS_ACL"

# ── T1: xattr round-trip ──────────────────────────────────────────────────────
echo "[T1] xattr round-trip (user.test=hello)"
if [ "$HAS_XATTR_OPT" -eq 0 ]; then
  skip "T1 xattr round-trip" "mutar built without xattr support"
elif [ "$FS_XATTR" -eq 0 ]; then
  skip "T1 xattr round-trip" "setfattr/getfattr missing or FS lacks user xattrs"
else
  printf 'payload\n' >"$SRC/xfile.txt"
  setfattr -n user.test -v hello "$SRC/xfile.txt"
  setfattr -n user.other -v world "$SRC/xfile.txt"
  TAR="$WORK/xattrs.tar"
  "$MUTAR" --posix --xattrs -cf "$TAR" -C "$SRC" xfile.txt
  if strings "$TAR" | grep -q 'SCHILY.xattr.user.test=hello'; then
    pass "T1 archive contains SCHILY.xattr.user.test=hello"
  else
    fail "T1 SCHILY.xattr in archive" "expected SCHILY.xattr.user.test=hello in $TAR"
  fi
  # security.selinux must never appear even if present on source
  if strings "$TAR" | grep -q 'SCHILY.xattr.security.selinux'; then
    fail "T1 no selinux" "security.selinux must not be stored"
  else
    pass "T1 security.selinux not stored"
  fi
  rm -rf "$OUT"/* && mkdir -p "$OUT"
  "$MUTAR" --xattrs -xf "$TAR" -C "$OUT"
  GOT="$(getfattr -n user.test --only-values "$OUT/xfile.txt" 2>/dev/null || true)"
  if [ "$GOT" = "hello" ]; then
    pass "T1 extract restores user.test=hello"
  else
    fail "T1 extract user.test" "got '$GOT' expected 'hello'"
  fi
  GOT2="$(getfattr -n user.other --only-values "$OUT/xfile.txt" 2>/dev/null || true)"
  if [ "$GOT2" = "world" ]; then
    pass "T1 extract restores user.other=world"
  else
    fail "T1 extract user.other" "got '$GOT2' expected 'world'"
  fi
fi

# ── T2: xattrs-include / xattrs-exclude ───────────────────────────────────────
echo "[T2] xattrs-include / xattrs-exclude filters"
if [ "$HAS_XATTR_OPT" -eq 0 ]; then
  skip "T2 filters" "mutar built without xattr support"
elif [ "$FS_XATTR" -eq 0 ]; then
  skip "T2 filters" "no FS xattr support"
else
  printf 'f\n' >"$SRC/filt.txt"
  setfattr -n user.keep -v yes "$SRC/filt.txt"
  setfattr -n user.drop -v no "$SRC/filt.txt"
  TAR="$WORK/filt.tar"
  "$MUTAR" --posix --xattrs \
    --xattrs-include='user.keep' \
    --xattrs-exclude='user.drop' \
    -cf "$TAR" -C "$SRC" filt.txt
  if strings "$TAR" | grep -q 'SCHILY.xattr.user.keep=yes'; then
    pass "T2 include keeps user.keep"
  else
    fail "T2 include" "user.keep missing from archive"
  fi
  if strings "$TAR" | grep -q 'SCHILY.xattr.user.drop'; then
    fail "T2 exclude" "user.drop should not be archived"
  else
    pass "T2 exclude drops user.drop"
  fi
  # exclude alone
  TAR2="$WORK/filt2.tar"
  "$MUTAR" --posix --xattrs --xattrs-exclude='user.drop' \
    -cf "$TAR2" -C "$SRC" filt.txt
  if strings "$TAR2" | grep -q 'SCHILY.xattr.user.keep=yes' \
     && ! strings "$TAR2" | grep -q 'SCHILY.xattr.user.drop'; then
    pass "T2 exclude-only filters correctly"
  else
    fail "T2 exclude-only" "unexpected SCHILY.xattr set in $TAR2"
  fi
fi

# ── T3: ACL round-trip ────────────────────────────────────────────────────────
echo "[T3] ACL round-trip (user:nobody:r--)"
if [ "$HAS_ACL_OPT" -eq 0 ]; then
  skip "T3 ACL round-trip" "mutar built without ACL support"
elif [ "$FS_ACL" -eq 0 ]; then
  skip "T3 ACL round-trip" "setfacl/getfacl missing or FS lacks ACLs"
else
  printf 'acl-payload\n' >"$SRC/afile.txt"
  setfacl -m u:nobody:r "$SRC/afile.txt"
  TAR="$WORK/acls.tar"
  "$MUTAR" --posix --acls -cf "$TAR" -C "$SRC" afile.txt
  if strings "$TAR" | grep -q 'SCHILY.acl.access='; then
    pass "T3 archive contains SCHILY.acl.access"
  else
    fail "T3 SCHILY.acl.access" "missing from $TAR"
  fi
  rm -rf "$OUT"/* && mkdir -p "$OUT"
  "$MUTAR" --acls -xf "$TAR" -C "$OUT"
  if getfacl -c --absolute-names "$OUT/afile.txt" 2>/dev/null | grep -q 'user:nobody:r'; then
    pass "T3 extract restores user:nobody ACL"
  else
    fail "T3 extract ACL" "user:nobody not present on extracted file"
    getfacl -c --absolute-names "$OUT/afile.txt" 2>&1 || true
  fi
fi

# ── T4: default ACL on directory ──────────────────────────────────────────────
echo "[T4] default ACL on directory"
if [ "$HAS_ACL_OPT" -eq 0 ]; then
  skip "T4 default ACL" "mutar built without ACL support"
elif [ "$FS_ACL" -eq 0 ]; then
  skip "T4 default ACL" "no FS ACL support"
else
  mkdir -p "$SRC/adir"
  setfacl -m u:nobody:rx "$SRC/adir"
  setfacl -d -m u:nobody:rwx "$SRC/adir"
  TAR="$WORK/dacl.tar"
  "$MUTAR" --posix --acls -cf "$TAR" -C "$SRC" adir
  if strings "$TAR" | grep -q 'SCHILY.acl.default='; then
    pass "T4 archive contains SCHILY.acl.default"
  else
    fail "T4 SCHILY.acl.default" "missing from $TAR"
  fi
  rm -rf "$OUT"/* && mkdir -p "$OUT"
  "$MUTAR" --acls -xf "$TAR" -C "$OUT"
  if getfacl -c --absolute-names "$OUT/adir" 2>/dev/null | grep -q 'default:user:nobody:rwx'; then
    pass "T4 extract restores default:user:nobody"
  else
    fail "T4 extract default ACL" "default:user:nobody missing"
    getfacl -c --absolute-names "$OUT/adir" 2>&1 || true
  fi
fi

# ── T5: --no-xattrs does not restore ──────────────────────────────────────────
echo "[T5] extract without --xattrs leaves attrs unset"
if [ "$HAS_XATTR_OPT" -eq 0 ] || [ "$FS_XATTR" -eq 0 ]; then
  skip "T5 no-xattrs extract" "xattr not available"
else
  printf 'n\n' >"$SRC/nx.txt"
  setfattr -n user.test -v secret "$SRC/nx.txt"
  TAR="$WORK/nx.tar"
  "$MUTAR" --posix --xattrs -cf "$TAR" -C "$SRC" nx.txt
  rm -rf "$OUT"/* && mkdir -p "$OUT"
  "$MUTAR" --no-xattrs -xf "$TAR" -C "$OUT" 2>/dev/null || "$MUTAR" -xf "$TAR" -C "$OUT"
  if getfattr -n user.test "$OUT/nx.txt" >/dev/null 2>&1; then
    fail "T5 no restore without --xattrs" "user.test present without --xattrs on extract"
  else
    pass "T5 extract without --xattrs does not restore"
  fi
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
exit 0

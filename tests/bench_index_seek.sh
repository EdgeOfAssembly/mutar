#!/usr/bin/env bash
# tests/bench_index_seek.sh — micro-benchmarks for index list + seek extract
#
# Not a pass/fail gate; prints timings. Exit 0 if benches complete.
# Usage: bash bench_index_seek.sh [/path/to/mutar]
set -euo pipefail

MUTAR="${1:-$(dirname "$0")/../build/mutar}"
N="${BENCH_MEMBERS:-2000}"
WORK="$(mktemp -d /tmp/mutar_bench.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo "=== mutar index/seek micro-benchmarks ==="
echo "binary: $MUTAR  members: $N"
echo

mkdir -p "$WORK/in"
for i in $(seq 1 "$N"); do
  printf 'b%05d\n' "$i" > "$WORK/in/f_$(printf '%05d' "$i").dat"
done
# Put target near the end
echo 'BENCH-TARGET' > "$WORK/in/zzz_target.dat"

time_ms() {
  # prints elapsed ms for command
  local start end
  start=$(date +%s%N)
  "$@" >/dev/null
  end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
}

echo "[setup] create uncompressed + index"
"$MUTAR" -cf "$WORK/plain.tar" --write-index -C "$WORK/in" .

echo "[setup] create xz --seekable + index"
if command -v xz >/dev/null; then
  "$MUTAR" -cJf "$WORK/x.tar.xz" --seekable -C "$WORK/in" . 2>/dev/null
fi

echo
echo "--- list $N members ---"
t_seq=$(time_ms env -u MUTAR_DEBUG_SEEK bash -c "
  mv '$WORK/plain.tar.mutaridx' '$WORK/plain.tar.mutaridx.bak'
  '$MUTAR' -tf '$WORK/plain.tar'
  mv '$WORK/plain.tar.mutaridx.bak' '$WORK/plain.tar.mutaridx'
")
t_idx=$(time_ms "$MUTAR" -tf "$WORK/plain.tar" --mutar-index="$WORK/plain.tar.mutaridx")
echo "  sequential list (no index): ${t_seq} ms"
echo "  index list:                 ${t_idx} ms"

echo
echo "--- extract one late member (plain) ---"
mkdir -p "$WORK/o1" "$WORK/o2"
t_ex_seq=$(time_ms env -u MUTAR_DEBUG_SEEK bash -c "
  mv '$WORK/plain.tar.mutaridx' '$WORK/plain.tar.mutaridx.bak'
  '$MUTAR' -xf '$WORK/plain.tar' -C '$WORK/o1' zzz_target.dat
  mv '$WORK/plain.tar.mutaridx.bak' '$WORK/plain.tar.mutaridx'
")
t_ex_idx=$(time_ms "$MUTAR" -xf "$WORK/plain.tar" --mutar-index="$WORK/plain.tar.mutaridx" \
  -C "$WORK/o2" zzz_target.dat)
echo "  sequential extract: ${t_ex_seq} ms"
echo "  index seek extract: ${t_ex_idx} ms"

if [[ -f "$WORK/x.tar.xz" ]]; then
  echo
  echo "--- extract one late member (xz + materialize) ---"
  mkdir -p "$WORK/o3"
  t_xz=$(time_ms "$MUTAR" -xJf "$WORK/x.tar.xz" --mutar-index="$WORK/x.tar.xz.mutaridx" \
    -C "$WORK/o3" zzz_target.dat)
  echo "  xz index+materialize extract: ${t_xz} ms"
fi

echo
echo "Done."

#!/usr/bin/env bash
# mock_rsh.sh — local stand-in for rsh/ssh used by mutar rmt tests.
# Ignores host/user and execs the remaining command (typically rmt).
# Usage: mock_rsh [-l user] host command [args...]
set -euo pipefail
while [ $# -gt 0 ]; do
  case "$1" in
    -l) shift 2 ;;
    -n|-f|-x|-q) shift ;;
    -*) shift ;;
    *) break ;;
  esac
done
if [ $# -lt 1 ]; then
  echo "mock_rsh: missing host" >&2
  exit 1
fi
shift # host
if [ $# -lt 1 ]; then
  echo "mock_rsh: missing remote command" >&2
  exit 1
fi
exec "$@"

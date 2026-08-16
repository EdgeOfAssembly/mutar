#!/usr/bin/env bash
# Wrapper: run mock_rmt.py (keeps --rmt-command path stable for tests)
exec python3 "$(dirname "$0")/mock_rmt.py" "$@"

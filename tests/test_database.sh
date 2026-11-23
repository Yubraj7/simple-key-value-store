#!/usr/bin/env bash
set -euo pipefail
BIN="./kvstore"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
DB="$TMP/data.db"

out="$(printf 'SET color blue\nGET color\nSET color green\nGET color\nGET missing\nEXIT\n' | "$BIN" --db "$DB")"
grep -q '^blue$' <<<"$out"
grep -q '^green$' <<<"$out"
grep -q '^(nil)$' <<<"$out"

out="$(printf 'GET color\n' | "$BIN" --db "$DB")"
grep -q '^green$' <<<"$out"

out="$(printf 'SET a 1\nSET b 2\nDEL a\nEXISTS a\nEXISTS b\nMSET c 3 d 4\nMGET c d\nINCR counter\nDECR counter\n' | "$BIN" --db "$DB")"
grep -q '^0$' <<<"$out"
grep -q '^1$' <<<"$out"
grep -q '^3$' <<<"$out"
grep -q '^4$' <<<"$out"
grep -q '^1$' <<<"$out"
grep -q '^0$' <<<"$out"

out="$(printf 'HSET profile name Ada\nHGET profile name\nHGETALL profile\nLPUSH nums two\nLPUSH nums one\nRPUSH nums three\nLRANGE nums 0 -1\n' | "$BIN" --db "$DB")"
grep -q '^Ada$' <<<"$out"
grep -q '^one$' <<<"$out"
grep -q '^two$' <<<"$out"
grep -q '^three$' <<<"$out"

out="$(printf 'BEGIN\nSET tx yes\nGET tx\nABORT\nGET tx\nBEGIN\nSET tx committed\nCOMMIT\n' | "$BIN" --db "$DB")"
grep -q '^yes$' <<<"$out"
grep -q '^(nil)$' <<<"$out"

out="$(printf 'GET tx\n' | "$BIN" --db "$DB")"
grep -q '^committed$' <<<"$out"

out="$(printf 'SET ttlkey value\nEXPIRE ttlkey 120\nTTL ttlkey\nRANGE a z\nFLUSHDB\nEXISTS b\n' | "$BIN" --db "$DB")"
grep -Eq '^[0-9]+$' <<<"$out"
grep -q '^0$' <<<"$out"

echo "All tests passed."

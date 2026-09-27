#!/bin/sh
# Compare our output with the reference implementation's golden files.
cd "$(dirname "$0")/.."
pass=0; fail=0
for f in test/golden/*.hpgf; do
  exp="${f%.hpgf}.output"
  got=$(node bin/heff.mjs golden "$f" 2>&1)
  if [ "$got" = "$(cat "$exp")" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $f"; fi
done
echo "golden: $pass passed, $fail failed"

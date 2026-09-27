#!/bin/sh
# show how the reference and the port differ on one file
REF=$(cat "$(dirname "$0")/../ref/REF_EXE")
f="$1"
"$REF" "$f" > /tmp/claude-501/r.out 2>/tmp/claude-501/r.err
node "$(dirname "$0")/../bin/heff.mjs" ref "$f" > /tmp/claude-501/o.out 2>/tmp/claude-501/o.err
echo "== stdout"; diff /tmp/claude-501/r.out /tmp/claude-501/o.out | head -${2:-15}
echo "== stderr"; diff /tmp/claude-501/r.err /tmp/claude-501/o.err | head -${2:-15}

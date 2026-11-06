#!/bin/bash
# CI helper: ci_run.sh <label> <command...> - runs the command, keeps its full output in build/ci-logs/<label>.log (uploaded
# as an artifact) and, when it fails, puts the tail of the output into a workflow annotation (annotations are readable
# without signing in; the raw job log isn't). Exit status is the command's.
label=$1; shift
mkdir -p build/ci-logs
log=build/ci-logs/$label.log
echo "::group::$label"
"$@" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
echo "::endgroup::"
if [ "$rc" != 0 ]; then
    tail -c 3500 "$log" | sed 's/\x1b\[[0-9;]*m//g' | python -c "
import sys
t=sys.stdin.read().replace('%','%25').replace('\r','').replace('\n','%0A')
print('::error title=$label failed (exit $rc)::'+t)" 2>/dev/null || echo "::error title=$label failed (exit $rc)::see build/ci-logs/$label.log"
fi
exit "$rc"

#!/bin/sh
# bisect-step.sh TEST - one `git bisect run` step: build, run TEST, report.
#
#   cp -r tools/regress ~/regress   # older commits have no tools/regress
#   git stash                       # bisect needs a clean tree
#   git bisect start <bad> <good>
#   git bisect run ~/regress/bisect-step.sh sndh44
#   git bisect reset && git stash pop
#
# Pi 5: REGRESS_MAKE="PIMODEL=PI5" git bisect run ...
# Exit 0 = good, 1 = bad, 125 = could not build (skip this commit).
test=${1:?usage: bisect-step.sh TEST}
here=$(cd "$(dirname "$0")" && pwd)
make -j2 $REGRESS_MAKE >/dev/null 2>&1 || exit 125
"$here/run.sh" "$test"

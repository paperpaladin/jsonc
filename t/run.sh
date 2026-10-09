#!/bin/sh
# t/run.sh -- build and run every oracle, print the conformance score and
# "N passed, M failed".
# SPDX-License-Identifier: GPL-3.0-or-later
#
# POSIX sh.  This script, not the Makefile, compiles the test programs, so
# `make test-asan` is nothing but a CFLAGS override.
set -u

cd "$(dirname "$0")/.." || exit 1

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -O2 -g}
SCRATCH=./scratch
mkdir -p "$SCRATCH"
OUT=$SCRATCH/results.txt
: > "$OUT"

say()  { printf '%s\n' "$*" | tee -a "$OUT"; }
ok()   { say "ok - $*"; }
nok()  { say "not ok - $*"; }

printf '=== building (CC=%s)\n' "$CC"
# shellcheck disable=SC2086
$CC $CFLAGS -o ./jsonc jsonc.c -lm || { nok "compile jsonc"; }

# --- hand-written accept/reject table --------------------------------------
# These duplicate a few JSONTestSuite cases on purpose: when the corpus is
# missing this section still fails if the parser breaks.
printf '\n--- hand-written accept/reject cases\n'
acc() { # acc <printf-format> <label>
	if printf '%b' "$1" | ./jsonc 2>/dev/null; then ok "accept $2"
	else nok "accept $2"; fi
}
rej() {
	if printf '%b' "$1" | ./jsonc 2>/dev/null; then nok "reject $2"
	else ok "reject $2"; fi
}

acc 'null'            'null'
acc 'true'            'true'
acc 'false'           'false'
acc ' \t\r\n null \n' 'literal wrapped in all four ws bytes'
rej ''                'empty input'
rej '   '             'whitespace only'
rej 'nul'             'truncated null'
rej 'nulll'           'null with trailing l'
rej 'NULL'            'uppercase NULL'
rej 'True'            'capitalised True'
rej 'null null'       'two top-level values'
rej '\013null'        'vertical tab is not JSON whitespace'
rej '\014null'        'form feed is not JSON whitespace'
rej '\000null'        'leading NUL byte'
rej '// c\nnull'      'line comment'
rej '/* c */null'     'block comment'

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

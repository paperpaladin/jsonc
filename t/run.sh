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

# --- strings: the escape set, control characters, UTF-8 ---------------------
# Case text goes through printf '%b', so a backslash in the JSON is doubled
# here and \NNN injects a raw byte.
printf '\n--- strings\n'
acc '""'                          'empty string'
acc '"abc"'                       'plain string'
acc '"\\""'                       'escaped quote'
acc '"\\\\"'                      'escaped backslash'
acc '"\\/"'                       'escaped solidus'
acc '"\\b\\f\\n\\r\\t"'           'the five single-letter control escapes'
acc '"\\u0041"'                   'escaped BMP codepoint'
acc '"\\u0000"'                   'escaped NUL inside a string'
acc '"\\uffff"'                   'escaped U+FFFF, lowercase hex'
acc '"\\uFFFF"'                   'escaped U+FFFF, uppercase hex'
acc '"\177"'                      'raw DEL is legal unescaped'
acc '"\302\242"'                  'raw 2-byte UTF-8'
acc '"\343\201\202"'              'raw 3-byte UTF-8'
acc '"\360\237\230\200"'          'raw 4-byte UTF-8'
acc '"\357\277\275"'              'raw U+FFFD'
rej '"'                           'lone quote'
rej '"abc'                        'unterminated string'
rej '"\\"'                        'string ending in a dangling escape'
rej '"\\x"'                       'unknown escape \\x'
rej '"\\'"'"'"'                   'single-quote escape'
rej '"\\u00"'                     'short \\u escape'
rej '"\\u00G0"'                   'non-hex digit in \\u escape'
rej '"\\u 000"'                   'space inside \\u escape'
rej '"\\u+000"'                   'sign inside \\u escape'
rej '"a\nb"'                      'raw newline in string'
rej '"a\011b"'                    'raw tab in string'
rej '"a\000b"'                    'raw NUL in string'
rej '"a\037b"'                    'raw 0x1F in string'
rej "'a'"                         'single-quoted string'
rej '"\300\257"'                  'overlong 2-byte encoding of /'
rej '"\340\200\257"'              'overlong 3-byte encoding of /'
rej '"\200"'                      'lone continuation byte'
rej '"\301\277"'                  'overlong lead byte 0xC1'
rej '"\365\200\200\200"'          'lead byte 0xF5, above U+10FFFF'
rej '"\364\220\200\200"'          'U+110000 in four bytes'
rej '"\377"'                      'byte 0xFF'
rej '"\355\240\200"'              'UTF-8-encoded high surrogate (CESU-8)'
rej '"\343\201"'                  'truncated 3-byte sequence'
rej '"\360\237\230"'              'truncated 4-byte sequence'
rej '"\\uD800"'                   'lone high surrogate escape'
rej '"\\uDC00"'                   'lone low surrogate escape'

# --- surrogate pairs -------------------------------------------------------
printf '\n--- surrogate pairs\n'
acc '"\\uD83D\\uDE00"'            'surrogate pair for U+1F600'
acc '"\\ud83d\\ude00"'            'surrogate pair, lowercase hex'
acc '"\\uD800\\uDC00"'            'lowest pair, U+10000'
acc '"\\uDBFF\\uDFFF"'            'highest pair, U+10FFFF'
acc '"a\\uD83D\\uDE00b"'          'pair surrounded by ASCII'
acc '"\\uD83D\\uDE00\\uD83D\\uDE00"' 'two pairs back to back'
rej '"\\uD800"'                   'high surrogate alone'
rej '"\\uDBFF"'                   'highest high surrogate alone'
rej '"\\uDC00"'                   'low surrogate alone'
rej '"\\uDFFF"'                   'highest low surrogate alone'
rej '"\\uD800\\uD800"'            'two high surrogates'
rej '"\\uDC00\\uDC00"'            'two low surrogates'
rej '"\\uDC00\\uD800"'            'reversed pair'
rej '"\\uD800\\u0041"'            'high surrogate then a BMP escape'
rej '"\\uD800x"'                  'high surrogate then a literal'
rej '"\\uD800\\n"'                'high surrogate then another escape'
rej '"\\uD800"'                   'high surrogate then end of string'
rej '"\\uD800\\uDC0"'             'pair with a short second escape'

# --- numbers ---------------------------------------------------------------
printf '\n--- numbers\n'
acc '0'                   'zero'
acc '-0'                  'negative zero'
acc '1'                   'one'
acc '-1'                  'minus one'
acc '123'                 'multi-digit integer'
acc '0.5'                 'fraction'
acc '-0.0'                'negative zero with a fraction'
acc '1e2'                 'exponent, no sign'
acc '1E2'                 'capital E'
acc '1e+2'                'exponent, plus'
acc '1e-2'                'exponent, minus'
acc '0e0'                 'zero with exponent'
acc '0e+1'                'zero with signed exponent'
acc '1.5e300'             'large but finite'
acc '-1.5e300'            'large negative but finite'
acc '5e-324'              'smallest subnormal double'
acc '1e-999'              'underflow to zero is not an error'
acc '-1e-999'             'underflow to negative zero'
acc '1.7976931348623157e308' 'DBL_MAX'
acc '123456789012345678901234567890' 'integer beyond 2^53, precision lost'
acc '0.000000000000000000000000001'  'many leading fractional zeros'
rej '1e999'               'overflow to +inf is rejected'
rej '-1e999'              'overflow to -inf is rejected'
rej '1e309'               'just past DBL_MAX'
rej '1.8e308'             'just past DBL_MAX, mantissa form'
rej '01'                  'leading zero'
rej '00'                  'two zeros'
rej '-01'                 'negative leading zero'
rej '+1'                  'leading plus'
rej '.1'                  'no integer part'
rej '1.'                  'no fractional digits'
rej '-'                   'lone minus'
rej '-.'                  'minus dot'
rej '1e'                  'exponent with no digits'
rej '1e+'                 'exponent sign with no digits'
rej '1E-'                 'capital exponent sign with no digits'
rej '1.e2'                'empty fraction before exponent'
rej '0x10'                'hex literal'
rej '0X10'                'capital hex literal'
rej '1_000'               'digit separator'
rej 'Infinity'            'Infinity'
rej '-Infinity'           'negative Infinity'
rej 'inf'                 'inf, which strtod would take'
rej 'NaN'                 'NaN'
rej 'nan'                 'nan, which strtod would take'
rej '1 2'                 'two numbers'
rej '--1'                 'double minus'
rej '1e1e1'               'two exponents'
rej '1.2.3'               'two decimal points'
rej '1"'                  'number then a quote'

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

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

# --- the API that the CLI cannot reach -------------------------------------
# Error codes, byte offsets, embedded NUL, duplicate-key lookup, the depth
# limit as a return value, and json_write into memory.  t/unit.c includes
# jsonc.c directly with JSONC_NO_MAIN.
printf '\n--- unit tests (t/unit.c)\n'
# shellcheck disable=SC2086
if $CC $CFLAGS -o ./t/unit t/unit.c -lm; then
	./t/unit 2>&1 | tee -a "$OUT"
else
	nok "compile t/unit"
fi

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

# --- arrays ----------------------------------------------------------------
printf '\n--- arrays\n'
acc '[]'                  'empty array'
acc '[ ]'                 'empty array with a space'
acc '[\n\t]'              'empty array with newline and tab'
acc '[1]'                 'one element'
acc '[1,2,3]'             'three elements'
acc '[ 1 , 2 ]'           'spaces around the comma'
acc '[[]]'                'array in array'
acc '[[[[[1]]]]]'         'five deep'
acc '[null,true,false]'   'literals as elements'
acc '["a",1,[],null]'     'mixed element types'
acc '[1e2,-0]'            'numbers as elements'
rej '['                   'unclosed array'
rej ']'                   'lone close bracket'
rej '[1'                  'unclosed after an element'
rej '[1,'                 'unclosed after a comma'
rej '[,]'                 'comma with no elements'
rej '[1,]'                'trailing comma'
rej '[,1]'                'leading comma'
rej '[1,,2]'              'double comma'
rej '[1 2]'               'missing comma'
rej '[1:2]'               'colon in an array'
rej '[]]'                 'extra close bracket'
rej '[}'                  'mismatched close brace'
rej '[1}'                 'mismatched close brace after an element'
rej '[[]'                 'unclosed outer array'
rej '[1,2'                'unclosed after two elements'
rej '[null'               'unclosed after a literal'
rej '["a"'                'unclosed after a string'
rej '[\f1]'              'form feed inside an array'

# --- objects ---------------------------------------------------------------
printf '\n--- objects\n'
acc '{}'                          'empty object'
acc '{ }'                         'empty object with a space'
acc '{"a":1}'                     'one member'
acc '{"a":1,"b":2}'               'two members'
acc '{ "a" : 1 }'                 'spaces around the colon'
acc '{"":1}'                      'empty key'
acc '{"a":{}}'                    'object in object'
acc '{"a":{"b":{"c":[]}}}'        'three deep, mixed'
acc '{"a":1,"a":2}'               'duplicate key is accepted'
acc '{"a":1,"a":1}'               'duplicate key with the same value'
acc '{"\\u0041":1}'               'escaped key'
acc '[{"a":1},{"b":2}]'           'objects in an array'
rej '{'                           'unclosed object'
rej '}'                           'lone close brace'
rej '{"a"'                        'key with no colon'
rej '{"a":'                       'colon with no value'
rej '{"a":1'                      'unclosed after a member'
rej '{"a":1,'                     'unclosed after a comma'
rej '{"a":1,}'                    'trailing comma'
rej '{,}'                         'comma with no members'
rej '{,"a":1}'                    'leading comma'
rej '{"a":1,,"b":2}'              'double comma'
rej '{"a":1 "b":2}'               'missing comma'
rej '{a:1}'                       'unquoted key'
rej "{'a':1}"                     'single-quoted key'
rej '{1:2}'                       'numeric key'
rej '{null:1}'                    'literal as key'
rej '{"a"=1}'                     'equals instead of colon'
rej '{"a"::1}'                    'double colon'
rej '{"a":1:2}'                   'colon after a member'
rej '{"a"}'                       'key with no value'
rej '{"a":1]'                     'mismatched close bracket'
rej '{{}}'                        'object as a key'
rej '{"a":1}}'                    'extra close brace'
rej '[{]'                         'unclosed object inside an array'

# --- the depth limit -------------------------------------------------------
# A crash is not a rejection, so these check the exit status exactly: 1 means
# "rejected", 134/139 would mean abort/segfault and must fail the suite.
printf '\n--- depth limit (JSONC_MAX_DEPTH)\n'
rep() { # rep <open> <n> <close> [innermost]
	python3 -c 'import sys
o, n, c = sys.argv[1], int(sys.argv[2]), sys.argv[3]
sys.stdout.write(o * n + (sys.argv[4] if len(sys.argv) > 4 else "") + c * n)' "$@"
}
depth_ok() { # depth_ok <open> <close> <n> <label>
	if rep "$1" "$3" "$2" "${5:-}" | ./jsonc 2>/dev/null; then ok "accept $4"
	else nok "accept $4"; fi
}
depth_rej() { # depth_rej <open> <close> <n> <label>
	rep "$1" "$3" "$2" "${5:-}" | ./jsonc 2>/dev/null
	st=$?
	if [ "$st" -eq 1 ]; then ok "reject $4 (exit 1)"
	else nok "reject $4 (exit $st, wanted 1)"; fi
}
depth_ok  '[' ']' 999    '999 nested arrays'
depth_ok  '[' ']' 1000   '1000 nested arrays, exactly at the limit'
depth_rej '[' ']' 1001   '1001 nested arrays'
depth_rej '[' ']' 100000 '100000 nested arrays'
depth_rej '[' ']' 1000000 '1000000 nested arrays'
depth_ok  '{"a":' '}' 999  '999 nested objects' 1
depth_rej '{"a":' '}' 1001 '1001 nested objects' 1
depth_rej '{"a":' '}' 100000 '100000 nested objects' 1
printf '[%.0s' $(seq 1 100000) | ./jsonc 2>/dev/null
st=$?
if [ "$st" -eq 1 ]; then ok "reject 100000 unclosed '[' (exit 1)"
else nok "reject 100000 unclosed '[' (exit $st, wanted 1)"; fi

# --- the serialiser: compact canonical form --------------------------------
printf '\n--- serialiser (-c)\n'
ser() { # ser <input> <expected-output>
	got=$(printf '%b' "$1" | ./jsonc -c 2>&1)
	want=$(printf '%b' "$2")
	if [ "$got" = "$want" ]; then ok "-c $1 -> $2"
	else nok "-c $1 -> $2 (got $got)"; fi
}
ser 'null'              'null'
ser 'true'              'true'
ser '  false  '         'false'
ser '0'                 '0'
ser '-0'                '-0'
ser '1'                 '1'
ser '1.0'               '1'
ser '1e2'               '100'
ser '100'               '100'
ser '1e15'              '1e+15'
ser '1e16'              '1e+16'
ser '12345678901234567890' '1.2345678901234567e+19'
ser '1e20'              '1e+20'
ser '0.1'               '0.1'
ser '0.3'               '0.3'
ser '1.7976931348623157e308' '1.7976931348623157e+308'
ser '5e-324'            '5e-324'
ser '1e-999'            '0'
ser '"abc"'             '"abc"'
ser '""'                '""'
ser '"\\u0041"'         '"A"'
ser '"\\u00e9"'         '"\303\251"'
ser '"\\uD83D\\uDE00"'  '"\360\237\230\200"'
ser '"\360\237\230\200"' '"\360\237\230\200"'
ser '"a\\nb"'           '"a\\nb"'
ser '"a\\u0000b"'       '"a\\u0000b"'
ser '"a\\u001fb"'       '"a\\u001fb"'
ser '"\\b\\f\\n\\r\\t"' '"\\b\\f\\n\\r\\t"'
ser '"\\/"'             '"/"'
ser '"\\""'             '"\\""'
ser '"\177"'            '"\177"'
ser '[]'                '[]'
ser '{}'                '{}'
ser '[ 1 , 2 ]'         '[1,2]'
ser '{ "a" : 1 }'       '{"a":1}'
ser '{"a":1,"a":2}'     '{"a":1,"a":2}'
ser '[[],{},null]'      '[[],{},null]'
ser '{"a":[1,{"b":[]}]}' '{"a":[1,{"b":[]}]}'

# --- round-trip: -c output must re-parse to itself -------------------------
printf '\n--- round-trip (parse -> write -> parse -> write)\n'
rt() {
	a=$(printf '%b' "$1" | ./jsonc -c 2>&1)
	b=$(printf '%s' "$a" | ./jsonc -c 2>&1)
	if [ "$a" = "$b" ] && [ -n "$a" ]; then ok "round-trip $1"
	else nok "round-trip $1 (first $a, second $b)"; fi
}
rt '{"a":[1,2,{"b":"\\u00e9\\uD83D\\uDE00"}],"c":[-0,1e20,0.1,5e-324]}'
rt '[[[[[[[[[[1]]]]]]]]]]'
rt '"\\u0000\\u001f\\"\\\\\177"'
rt '{"":{"":{"":[]}}}'
rt '[1e-999,-1e-999,1.7976931348623157e308]'

# --- the pretty printer, against CPython's json.dumps(indent=2) ------------
# Two independent implementations have to agree byte for byte.  Integer-valued
# numbers only here: Python writes floats with repr() and we write the shortest
# round-tripping %g, so 1e20 is "1e+20" to us and "1e+20" to repr but 1e15 is
# "1e+15" to us and "1000000000000000.0" to repr.  t/diff.py compares numbers
# under a shared rule instead.
printf '\n--- pretty printer vs CPython json.dumps(indent=2)\n'
pp() {
	got=$(printf '%s' "$1" | ./jsonc -p 2>&1)
	want=$(printf '%s' "$1" | python3 -c 'import sys, json
print(json.dumps(json.load(sys.stdin), indent=2, ensure_ascii=False), end="")' 2>&1)
	if [ "$got" = "$want" ]; then ok "-p agrees with json.dumps(indent=2) on $1"
	else nok "-p agrees with json.dumps(indent=2) on $1 (got [$got] want [$want])"; fi
}
pp '{}'
pp '[]'
pp 'null'
pp '123'
pp '"x"'
pp '[1,2,3]'
pp '{"a":1}'
pp '{"a":[1,2,{}],"b":[],"c":"x"}'
pp '[[],{},[[]],{"k":{}}]'
pp '{"a":{"b":{"c":{"d":[1,[2,[3]]]}}}}'
pp '["\u0000\u001f\u007f","\"","\\","/"]'
pp '["\u00e9\u4e2d\ud83d\ude00"]'
pp '{"":"","  ":"\t\n\r\b\f"}'
pp '[true,false,null,0,-1]'

# Known divergence, asserted so that a change on either side is noticed:
# CPython's json reads "-0" as the *integer* 0 and loses the sign.  We read
# every number as a double, so -0 stays -0.  Neither is wrong (RFC 8259 says
# nothing about signed zero) but they cannot be byte-compared, so this is the
# one number the pretty-print comparison above leaves out.
got=$(printf '%s' '-0' | ./jsonc -c)
want=$(python3 -c 'import json; print(json.dumps(json.loads("-0")))')
if [ "$got" = "-0" ] && [ "$want" = "0" ]; then
	ok "divergence confirmed: jsonc -0 keeps the sign, CPython json gives 0"
else
	nok "divergence confirmed: jsonc gave $got, CPython gave $want"
fi

# --- JSONTestSuite conformance --------------------------------------------
# github.com/nst/JSONTestSuite, MIT, Nicolas Seriot.  Vendored as a tarball
# under t/ so this does not need the network; see README for provenance.
#   y_*  must be accepted
#   n_*  must be rejected
#   i_*  either answer is conformant, but the answer must be consistent, so
#        ours is pinned in t/i_choices.expected and diffed.
# Any exit status other than 0 or 1 is a crash and counts as a failure
# whatever the filename says.
printf '\n--- JSONTestSuite conformance (test_parsing)\n'
CORPUS=$SCRATCH/test_parsing
if [ ! -d "$CORPUS" ]; then
	tar xzf t/JSONTestSuite-test_parsing.tar.gz -C "$SCRATCH" 2>/dev/null \
		|| nok "extract t/JSONTestSuite-test_parsing.tar.gz"
fi
if [ ! -d "$CORPUS" ]; then
	nok "JSONTestSuite corpus present"
else
	yp=0; yf=0; np=0; nf=0; ia=0; ir=0; crash=0
	: > "$SCRATCH/i_choices.txt"
	: > "$SCRATCH/conformance_failures.txt"
	for f in "$CORPUS"/*.json; do
		base=${f##*/}
		msg=$(./jsonc "$f" 2>&1)
		st=$?
		if [ "$st" -ne 0 ] && [ "$st" -ne 1 ]; then
			crash=$((crash + 1))
			printf '%s\tEXIT %s\t%s\n' "$base" "$st" "$msg" \
				>> "$SCRATCH/conformance_failures.txt"
			continue
		fi
		case $base in
		y_*)
			if [ "$st" -eq 0 ]; then yp=$((yp + 1)); else
				yf=$((yf + 1))
				printf '%s\tWRONGLY REJECTED\t%s\n' "$base" "$msg" \
					>> "$SCRATCH/conformance_failures.txt"
			fi ;;
		n_*)
			if [ "$st" -eq 1 ]; then np=$((np + 1)); else
				nf=$((nf + 1))
				printf '%s\tWRONGLY ACCEPTED\t-\n' "$base" \
					>> "$SCRATCH/conformance_failures.txt"
			fi ;;
		i_*)
			if [ "$st" -eq 0 ]; then
				ia=$((ia + 1))
				printf '%s\taccept\n' "$base" >> "$SCRATCH/i_choices.txt"
			else
				ir=$((ir + 1))
				printf '%s\treject\n' "$base" >> "$SCRATCH/i_choices.txt"
			fi ;;
		*)
			nok "unexpected corpus filename $base" ;;
		esac
	done
	say "SCORE y_ accepted: $yp/$((yp + yf))"
	say "SCORE n_ rejected: $np/$((np + nf))"
	say "SCORE i_ choices:  $((ia + ir)) made ($ia accepted, $ir rejected)"
	if [ "$crash" -eq 0 ]; then ok "no corpus file produced an exit status outside {0,1}"
	else nok "$crash corpus files crashed (see scratch/conformance_failures.txt)"; fi
	if [ "$yf" -eq 0 ]; then ok "all y_ files accepted ($yp)"
	else nok "$yf y_ files wrongly rejected"; fi
	if [ "$nf" -eq 0 ]; then ok "all n_ files rejected ($np)"
	else nok "$nf n_ files wrongly accepted"; fi
	if [ -s "$SCRATCH/conformance_failures.txt" ]; then
		printf '\nconformance failures:\n'
		cat "$SCRATCH/conformance_failures.txt"
	fi
	# The i_ answers are implementation-defined but must not drift silently.
	if [ -f t/i_choices.expected ]; then
		if diff -u t/i_choices.expected "$SCRATCH/i_choices.txt" > "$SCRATCH/i.diff" 2>&1
		then ok "i_ answers match t/i_choices.expected"
		else nok "i_ answers changed"; cat "$SCRATCH/i.diff"; fi
	else
		nok "t/i_choices.expected missing (copy scratch/i_choices.txt)"
	fi
fi

# --- the truncation sweep --------------------------------------------------
# Every prefix of every corpus file, parsed and freed.  The verdicts are
# meaningless; the point is that no truncated document crashes, overruns or
# leaks.  Under `make test-asan` this is where that gets proved.
printf '\n--- truncation sweep over the corpus\n'
if [ -d "$CORPUS" ]; then
	bad=0
	total=0
	for f in "$CORPUS"/*.json; do
		out=$(./jsonc -S "$f" 2>&1)
		st=$?
		if [ "$st" -ne 0 ]; then
			bad=$((bad + 1))
			printf 'SWEEP FAILED exit %s: %s\n%s\n' "$st" "${f##*/}" "$out"
		else
			n=${out#swept }
			total=$((total + ${n%% *}))
		fi
	done
	say "SWEEP $total truncated documents parsed across 318 corpus files"
	if [ "$bad" -eq 0 ]; then ok "every prefix of every corpus file parsed without crashing"
	else nok "$bad corpus files crashed during the truncation sweep"; fi
fi

# Hand-written sweeps for the shapes the corpus does not contain: an escape cut
# in half, a surrogate pair cut between the two escapes, a UTF-8 sequence cut
# mid-continuation, and a number cut after its exponent sign.
printf '\n--- truncation sweep over hand-written edge shapes\n'
for s in '"\\uD83D\\uDE00"' '"\\u0041"' '"\343\201\202"' '[1e+10]' '{"a":[-0.5e-7]}' '"\\\\\\""' '[[[[[[[[[[1]]]]]]]]]]' '{"a":{"b":{"c":1}}}'; do
	printf '%b' "$s" > "$SCRATCH/sweep.json"
	if ./jsonc -S "$SCRATCH/sweep.json" > /dev/null 2>&1; then
		ok "sweep $s"
	else
		nok "sweep $s"
	fi
done

# --- random bytes ----------------------------------------------------------
# Not an oracle, a smoke test: 400kB of urandom must be rejected, not crash.
printf '\n--- hostile input\n'
head -c 400000 /dev/urandom > "$SCRATCH/random.bin"
./jsonc "$SCRATCH/random.bin" > /dev/null 2>&1
st=$?
if [ "$st" -eq 0 ] || [ "$st" -eq 1 ]; then ok "400kB of /dev/urandom: exit $st"
else nok "400kB of /dev/urandom: exit $st"; fi
./jsonc -S "$SCRATCH/random.bin" > /dev/null 2>&1
st=$?
if [ "$st" -eq 0 ]; then ok "truncation sweep over 400kB of /dev/urandom"
else nok "truncation sweep over 400kB of /dev/urandom (exit $st)"; fi

# --- the differential oracle: CPython's json module ------------------------
# A second implementation, by other people, over the same corpus plus 120
# generated documents and a named edge battery.  Its ok/not-ok lines are tee'd
# into the tally like everything else.
printf '\n--- differential oracle vs CPython json (t/diff.py)\n'
python3 t/diff.py ./jsonc "$CORPUS" 2>&1 | tee -a "$OUT"

# --- valgrind (only when asked: `make test-valgrind`) ----------------------
# ASan and valgrind find overlapping but not identical things, and valgrind
# needs an uninstrumented binary, which is why this is a separate target
# rather than part of the default run.  t/unit exercises every free path in
# one process; the -S runs are where a read of uninitialised or freed memory
# would show up.
if [ "${JSONC_VALGRIND:-}" = 1 ]; then
	printf '\n--- valgrind\n'
	VG="valgrind --error-exitcode=9 --leak-check=full --errors-for-leak-kinds=all -q"
	if $VG ./t/unit > /dev/null 2>"$SCRATCH/vg.err" && [ ! -s "$SCRATCH/vg.err" ]; then
		ok "valgrind clean on t/unit"
	else
		nok "valgrind on t/unit"; cat "$SCRATCH/vg.err"
	fi
	for f in n_structure_100000_opening_arrays.json \
		 n_structure_open_array_object.json \
		 i_structure_500_nested_arrays.json \
		 y_object_duplicated_key.json \
		 y_string_utf8.json \
		 y_string_accepted_surrogate_pairs.json \
		 i_string_truncated-utf-8.json; do
		if [ ! -f "$CORPUS/$f" ]; then
			nok "valgrind: corpus file $f is missing, test skipped"
			continue
		fi
		if $VG ./jsonc -S "$CORPUS/$f" > /dev/null 2>"$SCRATCH/vg.err" \
			&& [ ! -s "$SCRATCH/vg.err" ]; then
			ok "valgrind clean sweeping $f"
		else
			nok "valgrind sweeping $f"; cat "$SCRATCH/vg.err"
		fi
	done
	if $VG ./jsonc -S "$SCRATCH/random.bin" > /dev/null 2>"$SCRATCH/vg.err" \
		&& [ ! -s "$SCRATCH/vg.err" ]; then
		ok "valgrind clean sweeping 400kB of /dev/urandom"
	else
		nok "valgrind sweeping random bytes"; cat "$SCRATCH/vg.err"
	fi
fi

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

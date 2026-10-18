# jsonc

A strict RFC 8259 JSON parser and serialiser in one C11 file, plus a CLI that
validates with its exit status and pretty-prints.  No dependencies beyond
libc.  1112 lines including the comments.

The name is unfortunate and deliberate: this is not "JSON with comments".
Comments are rejected.  So are trailing commas, unquoted keys, single-quoted
strings, `NaN`, `Infinity`, hex numbers, leading `+`, leading zeros, bare
control characters in strings, lone surrogates and ill-formed UTF-8.

## The score, and the only interesting claim

Anyone can write a JSON parser and assert that it is conformant.  The claim is
unverifiable without a number from a corpus the author did not write, so here
is the number, from
[JSONTestSuite](https://github.com/nst/JSONTestSuite) (Nicolas Seriot, MIT),
the standard conformance corpus for JSON parsers:

```
SCORE y_ accepted: 95/95        (must-accept files)
SCORE n_ rejected: 188/188      (must-reject files)
SCORE i_ choices:  35 made (6 accepted, 29 rejected)
```

**No failures.  Nothing is excluded, nothing is skipped, and the `i_` answers
are pinned.**  The corpus is `test_parsing/` from the master branch, vendored
here as a 12KB tarball (`t/JSONTestSuite-test_parsing.tar.gz`, 318 files,
354KB uncompressed) with its MIT licence beside it in
`t/JSONTestSuite-LICENSE`, so `make test` scores against it without a network.
Only `test_parsing/` is vendored; the suite's own test runners, its
`test_transform/` directory and its `parsers/` tree are not.

The runner counts **any exit status outside {0,1} as a failure regardless of
what the filename asks for**.  That matters more than the score: a segfault on
`n_structure_100000_opening_arrays.json` satisfies "did not exit 0", so a
harness written as `! ./jsonc "$f"` scores a crash as a pass.  Ours does not.

That was true of the corpus loop and **false of the hand-written table** until
it was fixed.  `rej()` was written `if ... ; then nok; else ok; fi`, and `if
cmd` is true only on exit 0, so every nonzero status counted as a successful
rejection, 134 for an abort and 139 for a segfault included, across roughly
150 reject cases.  It was wrong in the one section whose stated job is to
still catch a broken parser when the corpus is absent, and the only one
covering the malformed-UTF-8 and surrogate inputs.

Demonstrated by making `jsonc` call `abort()` where it returns 1.  On five
reject cases the old helper reported `ok` five times; the new one reports
`EXIT 134` five times.  Both helpers now compare the status to an exact
value and name the number when it is neither.

`i_*` files are implementation-defined, either answer conforms, but the
answer has to be consistent, so all 35 are recorded in `t/i_choices.expected`
and diffed on every run.  Changing one now fails the suite.  The choices:

| `i_` family | our answer | why |
| --- | --- | --- |
| lone surrogate escapes (10 files) | reject | an unpaired surrogate is not a Unicode scalar value, so there is no UTF-8 for it; the alternatives are CESU-8, U+FFFD substitution, or emitting `ED A0 80`, which our own parser then refuses to read back |
| ill-formed UTF-8 (11 files) | reject | overlong forms, truncated sequences, continuation bytes alone, UTF-8-encoded surrogates, codepoints above U+10FFFF, Latin-1 bytes |
| number overflow to infinity (5 files) | reject | keeps the serialiser total: no tree that parses can fail to serialise as valid JSON |
| number underflow, huge-but-finite integers (5 files) | **accept** | precision loss, which RFC 8259 section 6 explicitly permits |
| UTF-16 input, leading UTF-8 BOM (3 files) | reject | RFC 8259 section 8.1: "Implementations MUST NOT add a byte order mark" and the text is UTF-8 |
| 500 nested arrays | **accept** | under the depth limit of 1000 |

## Second oracle: CPython's `json`

```
DIFF accept/reject agreement with CPython json: 295/318 files
```

23 disagreements, all pinned in `t/py_divergences.expected`, and **all 23 in
the same direction: CPython accepts something we reject.**  There is no file we
accept and CPython rejects, the direction that would be alarming.
What CPython is lenient about:

- `NaN`, `Infinity` and `-Infinity` as bare tokens.  These are not JSON, and
  `json.dumps` writes them back out as those same invalid tokens.
- lone surrogates in `\u` escapes, which it decodes into unpaired surrogates
  in a `str` that then cannot be encoded as UTF-8.
- numbers that overflow to `inf`.
- a leading UTF-8 BOM.
- UTF-16 and UTF-32 input, because `json.loads(bytes)` sniffs BOMs.

Beyond accept/reject, `t/diff.py` compares *values*: both implementations
parse the same document, both canonicalise to text under one shared rule, and
the text is compared.  99 of the 101 mutually accepted corpus files are
byte-identical; 120 generated documents and a 17-case edge battery (random
64-bit mantissas, DBL_MAX, both subnormal extremes, 2^53 and 2^53+1, a 1MB
string, a 100k-element array, a 10k-key object, 400-deep nesting, every escape
form, surrogate pairs at both ends of the astral range, a sampled sweep of
U+0020 to U+10FFFF) all match in both directions.

The 2 exceptions are `y_number_minus_zero` and `y_number_negative_zero`:
CPython reads `-0` as the integer `0` and loses the sign.  We read every
number as a `double` and print `-0`.  RFC 8259 says nothing about signed zero,
so neither is wrong, but the two cannot be byte-compared.  Those two filenames
are excused individually and the count is asserted. A blanket "ignore signed
zero" rule would have hidden a real sign bug anywhere else.

## Third oracle: the sanitizers over a truncation sweep

```
SWEEP 12605 truncated documents parsed across 318 corpus files
```

`jsonc -S FILE` parses every prefix of the file and frees the result.  The
verdicts are meaningless by design: a prefix of a valid document is usually
invalid, the point is that every "is there another byte?" check in the parser
gets exercised at the exact offset where the answer is no.  `make test-asan`
runs the whole suite under `-fsanitize=address,undefined` with
`detect_leaks=1`; `make test-valgrind` runs valgrind over `t/unit` and eight
sweeps with `--errors-for-leak-kinds=all`.  Both clean.

**The sweep was useless when it was first written, and that is the most useful
thing in this repo.**  `slurp()` allocated 64KB up front, so for a five-byte
document every read past the end of the *document* landed in slack inside the
same allocation, and no error AddressSanitizer can see.  Deleting the
four-byte length check in `hex4()`: a guaranteed out-of-bounds read on
`"\u00`, produced a clean sanitizer run and exit 0.  Two one-line fixes:
`slurp()` now reallocs down to the exact byte count, and `sweep()` copies each
prefix into its own `malloc(off)` instead of passing a short length into a long
buffer.  With those, the same deliberate breakage reports
`heap-buffer-overflow ... READ of size 1 ... in hex4`.

A clean sanitizer run against a buffer with slack is not evidence of anything.

## The arena, and what it costs the sanitizer

`json_parse` bump-allocates the entire tree out of one arena: every node,
every decoded string, every array and object vector.  `json_free` releases it
by walking a short list of blocks.  Before, it was one `calloc` per node, one
`malloc` per string, one `realloc` chain per vector, and a recursive
`json_free` that matched them one for one.

Measured, not asserted.  Instruction counts are from
`valgrind --tool=callgrind` on three generated documents in `scratch/bench`,
wall clock is the median of eleven runs of the CLI:

| input | Ir before | Ir after | allocator Ir before | after | wall before | after |
|---|---|---|---|---|---|---|
| 50k records, 5.5MB (objects and short strings) | 699,233,648 | 318,024,057 | 385,329,838 (55%) | 317,703 (0.1%) | 0.11s | 0.05s |
| 200k strings, 9.9MB | 592,451,495 | 369,184,853 | 192,514,818 (32%) | 12,256 (0.0%) | 0.08s | 0.05s |
| 200k numbers, 3.6MB | 350,623,062 | 283,278,910 | 67,976,964 (19%) | 609,330 (0.2%) | 0.05s | 0.04s |

`jsonc -S` over all 318 corpus files, 12605 truncated parses, goes from 0.94s
to 0.55s (median of seven).  The number-heavy case moves least because
`strtod` and the mpn routines under it are 68% of what is left, and the
arena does not touch them.  Peak RSS falls
as well, 62.7MB to 41.2MB on the records document, because a node no longer
carries malloc's per-chunk header.

The plan item that asked for this said to measure before building, because
"nothing here is currently performance-bound".  The measurement is the 55%
above: on the object-and-string workload the allocator was the majority of
the work.

The second reason is that there is no partial-free path left in the parser.
`parse_object` used to free a key on one error branch and `json_free` a
half-built value on another; `obj_push` and `arr_push` each had to dispose of
what they had just been handed when a `realloc` failed.  Twenty-one lines calling
`free` or `json_free` are gone from `jsonc.c`, along with the two `bad:`
labels in the container parsers that they served, replaced by `return NULL`.

### What it costs

Two neighbouring nodes now live inside one malloc'd block, so **ASan cannot
see a write that runs off the end of one node into the next.**  It still sees
a write that runs off the end of the whole block, and `make test-asan` proves
that it does: an arena that never allocates a second block reports
`heap-buffer-overflow`.  Between objects, nothing.

That is the same blindness as a read buffer with slack in it, which this
collection has found twice before, and it is not undone by noting it here.
So there is a second allocation mode.  `-DJSONC_ARENA_DEBUG` gives every
object its own exactly sized `malloc`, which puts a redzone back between
neighbours; `arena_grow` then always moves the object, so a pointer kept
across a growth is a stale pointer rather than one that happened to stay
valid; and `arena_trim` reallocs down to the string's real length rather than
leaving the doubling slack in place.  `t/run.sh` builds the CLI and `t/unit`
both ways and runs the corpus, the truncation sweep and the whole unit suite
in both.  The two modes must agree byte for byte on all 318 corpus files,
which is checked rather than assumed.

Two cases in the suite exist only to watch those redzones fire: a read one
byte past a decoded string and a read one byte past a node, each required to
produce `heap-buffer-overflow` under `-DJSONC_ARENA_DEBUG`.  Removing the
debug-mode `realloc` from `arena_trim` turns the first red; over-allocating by
32 bytes in the debug allocator turns the second red.

**The string case was already blind before the arena existed.**  `buf` rounded
a six-byte string up to a sixteen-byte `malloc` and never trimmed it, so a
read one past the NUL landed in slack and ASan said nothing, exactly the bug
the sweep section below describes in `slurp`.  The same probe built against
the pre-arena commit is silent.  The debug allocator is the first thing in
this repo that catches it.

### Alignment

A misaligned arena allocation works on x86-64 for every type this file
allocates and is undefined behaviour regardless, so it is checked in two ways
that are independent of each other.  `t/unit.c` parses a document that
interleaves odd-length strings with numbers and objects, and asserts that
every node, every element vector and every member vector is aligned for its
type.  That assertion fails on its own, with no sanitizer.  Separately, the
whole suite runs under `-fsanitize=undefined`, and deleting the round-up in
`arena_alloc` produces
`runtime error: member access within misaligned address ... for type 'struct json'`.
Both were watched failing, which is the only reason to believe either.

### Mutations, including the one that survives

Eight defects were planted in the arena and the gate run against each.  Seven
turn `make test` or `make test-asan` red: overlapping allocations (the suite
segfaults), the alignment round-up deleted, a block that never grows,
`arena_grow` not copying the bytes it was asked to preserve, `arena_trim`
giving back the string instead of the slack, the debug allocator
over-allocating by 32 bytes, and `arena_trim` not shrinking under the debug
allocator.

**One survives `make test`.**  Making `arena_destroy` skip the chain of blocks
after the first leaks everything a document larger than 4KB allocated, and
plain `make test` is entirely green.  It is caught by `make test-asan`, where
LeakSanitizer reports it, and by `make test-valgrind`.  There is no
uninstrumented check for it, because a leak is not observable from a process
that is about to exit.  If you change the arena, run `make test-asan`: the
default target is not sufficient for that class of defect and never was.

## Build and use

```sh
make            # gcc -std=c11 -Wall -Wextra -Wpedantic -O2
make test       # 386 passed, 0 failed
make test-asan  # same 386, under -fsanitize=address,undefined
make test-valgrind   # 395: the nine valgrind cases on top
```

```
usage: jsonc [-p|-c|-S] [FILE]
  (no flag)  validate only: exit 0 if valid, 1 if not, 2 if unreadable
  -p         pretty-print (2-space indent)
  -c         compact canonical serialisation
  -S         truncation sweep: parse every prefix, report the count
```

Reads `FILE` or stdin.  On rejection it writes one line to stderr and nothing
to stdout:

```
$ printf '{\n  "a": 01\n}' | jsonc
jsonc: <stdin>:2:9: malformed number (byte 10)
```

Line and column for humans, byte offset in brackets because it is the only
coordinate the API returns.

As a library there is no header: `#define JSONC_NO_MAIN` and `#include
"jsonc.c"`, as `t/unit.c` does.

```c
json *root;
size_t errpos;
json_err e = json_parse(buf, len, &root, &errpos);
if (e != JSON_OK)
        fprintf(stderr, "%s at byte %zu\n", json_strerror(e), errpos);
else {
        const json *v = json_get(root, "key");   /* objects */
        size_t n = json_len(root);               /* arrays and objects */
        const json *first = json_at(root, 0);    /* arrays */
        json_write(stdout, root, 2);             /* 0 = compact */
        json_free(root);
}
```

## Decisions you have to know about

Every number is a `double`. RFC 8259 section 6 permits this and says
IEEE 754 binary64 is what to expect.  `123456789012345678901` parses, and
loses bits.  The original spelling is not kept: `1.0` serialises as `1`, `1e2`
as `100`, `1e20` as `1e+20`.  If you need int64 fidelity, this is the wrong
parser and no flag will fix it.

Numbers that overflow to infinity are rejected. `1e999` is an error
(`JSON_ERR_RANGE`).  Underflow is not: `1e-999` is `0.0`.  The reason is that
it keeps the serialiser total, if an infinity could live in the tree, there
would exist a tree that parses and cannot be written back as valid JSON, which
is exactly the hole CPython has.

Duplicate keys are kept, all of them, in document order. `json_get`
returns the last match, like JavaScript and Python.  `{"a":1,"a":2}`
serialises back to `{"a":1,"a":2}`, not `{"a":2}`.  RFC 8259 section 4 permits
any of keep-last, keep-first or error. Accepting the document is the part that
is not optional, and a parser that errors on duplicates fails
`y_object_duplicated_key`.

Unpaired surrogates are rejected. Both halves: `"\ud800"` alone and
`"\udc00"` alone, and a high surrogate followed by anything other than
`\uDC00-\uDFFF`.

The canonical number form is the shortest of `%.1g` .. `%.17g` that reads
back as the same double.  `%.17g` alone prints `0.1` as
`0.10000000000000001`; taking the *first* precision that round-trips rather
than the shortest *result* prints `100` as `1e+02`, because `%g` picks
exponential notation when the decimal exponent is at least the precision.
`t/diff.py` implements the identical rule in Python, and that lets the two
compare serialised text instead of parsed values.

`JSONC_MAX_DEPTH` is 1000, compile-time only.  Deeper is
`JSON_ERR_DEPTH`, not a crash: rebuilt with `-DJSONC_MAX_DEPTH=100000000` and
fed 1000000 nested arrays, the binary dies with SIGSEGV, exit 139.  With the
limit it exits 1.

## What does not work

- No streaming and no incremental parsing. One buffer in, one tree out.
  The whole document is in memory anyway because the DOM is, but there is no
  way to parse a 4GB file or to feed bytes as they arrive.
- No way to build or mutate a tree. You can parse and you can serialise
  what you parsed.  There is no `json_new_object`, no `json_set`, no
  `json_append`.  The serialiser exists to prove the parser round-trips, and never as a writer API.
- No int64, no bignum, no original-spelling preservation. See above.  This
  rules out signing JWTs, canonical JSON per RFC 8785, and anything that
  compares numbers textually.
- The C locale is assumed. `strtod` and `snprintf("%g")` both honour
  `LC_NUMERIC`, so a *library* caller that has done
  `setlocale(LC_ALL, "de_DE.UTF-8")` will get a parser that wants `0,5` and a
  serialiser that emits it.  The CLI is safe because it never calls
  `setlocale`.  This is untested: no comma-decimal locale is installed on
  this machine, so the claim is read off the `strtod(3)` and `printf(3)`
  specs, not demonstrated.  `strtod_l`/`uselocale` would fix it and would
  cost the "libc only, any libc" property.
- `json_get` is a linear scan, so looking up n keys in an n-key object is
  O(n^2).  Marked `shortcut:` in the source.  The CLI never calls it.
- The truncation sweep steps for large inputs. Over 8KB it cuts at `len/4096` boundaries instead of every byte, which affects 3 of the 318
  corpus files.  Marked `shortcut:`.
- `-p` only does 2-space indent. `json_write` takes any width; the CLI
  does not expose it, because the only consumer that mattered was the
  byte-comparison against `json.dumps(indent=2)`.
- Errors stop at the first one. There is no error recovery and no list of
  all problems in a document.
- The arena blinds ASan between neighbouring objects. Covered at length
  above.  `-DJSONC_ARENA_DEBUG` is the answer and the suite runs it, but the
  binary you ship is the blind one.
- `json_free` may only be called on a pointer `json_parse` returned. The
  root node is a member of the arena header and `json_free` finds the arena
  by stepping back with `offsetof`, so handing it a subtree is undefined.  It
  was never meaningful before either (it left the parent holding freed
  children) but it used to be merely wrong rather than undefined.
- The arena never reuses anything inside one parse. Each doubling of an
  array or object vector abandons the previous one in the arena, so the peak
  cost of an n-element vector is about 2n slots.  Total peak RSS still fell,
  because malloc's per-chunk header on every node cost more than the
  abandoned vectors do, but a pathological document could invert that.
- `-DJSONC_ARENA_DEBUG` costs memory. 83.5MB against 41.2MB on the records
  document, because every object carries a malloc header and a redzone again.
  It is a test mode, not a build option worth shipping.
- There is no way to supply your own allocator. `arena_alloc` calls
  `malloc` directly.  A `json_parse_with` taking an allocator struct is the
  obvious upgrade and nobody has asked for it.
- gcc and C11 only. Builds with clang too, but that is not tested in
  `make test`.  `t/unit.c` uses POSIX `open_memstream`, so the *tests* are not
  portable off POSIX even though `jsonc.c` is.
- `make test` needs `python3` for the differential oracle and for
  generating the deep-nesting cases.  Without it the C-only sections still
  run but the suite will report failures instead of skipping cleanly.
- No fuzzer. 400kB of `/dev/urandom` is a smoke test, not coverage-guided
  fuzzing.  The truncation sweep plus the corpus is what stands in for it.

## Oracles, ranked by how little we control them

1. JSONTestSuite. 318 files, not ours, built out of the inputs that broke
   real parsers.
2. CPython's `json`. Independent implementation, independent authors, and
   it disagrees with us in 23 places that then had to be justified.
3. AddressSanitizer, UndefinedBehaviorSanitizer, valgrind. Not correctness
   oracles, but the only authority on memory safety that is not our own
   opinion.
4. Round-trip. parse -> write -> parse -> write must be a fixed point.
   Self-consistency only, so it is ranked last, but it is the only check that
   tests the serialiser against the parser.

Each of the four has been watched failing on purpose, and each failure is
recorded in the commit that introduced the check: `isspace(3)` instead of the
four RFC whitespace bytes (2 failures), a trailing comma allowed in
`parse_array` (score drops to 186/188), `write_number` pinned to 17 digits
(5 value mismatches against CPython), the depth limit removed (exit 139), and
the `hex4` length check removed (clean run until `slurp` was fixed, then a
heap-buffer-overflow report).

## Licence

GPL-3.0-or-later. See `LICENSE`.  `t/JSONTestSuite-test_parsing.tar.gz` is
test data from github.com/nst/JSONTestSuite, Copyright (c) 2016 Nicolas
Seriot, MIT, included verbatim; see `t/JSONTestSuite-LICENSE`.

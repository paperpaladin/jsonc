# PLAN

## Contract

`jsonc` is one C11 file that parses RFC 8259 JSON into a DOM, serialises a DOM
back to JSON, and ships a CLI around both.

- `json_parse(buf, len, &root, &errpos)` returns `JSON_OK` or an error code
  plus the byte offset of the byte that broke the parse.  No callbacks, no
  streaming, no incremental feed: one buffer in, one tree out.
- `json_write(FILE *, root, indent)` serialises; `indent == 0` is compact,
  `indent > 0` is pretty-printed with that many spaces per level.
- `json_free(root)` releases it.  `json_get(obj, key)`, `json_len`,
  `json_at` are the three accessors the CLI and the tests actually needed.
- `jsonc FILE` validates and says so with its exit status and nothing else;
  `-p` pretty-prints, `-c` writes the compact canonical form, `-S` runs the
  truncation sweep.
- Not in the contract: JSON5, JSONC-with-comments (the name is a coincidence
  and a bad one), JSON Pointer, Patch, Schema, streaming/SAX, mutation of a
  parsed tree, preserving the original spelling of a number, int64 preservation
  (every number is a `double`), or any locale other than C.

## Hard invariant

**Acceptance is exactly the RFC 8259 grammar, and no input can make the parser
crash, read out of bounds, leak, or recurse without bound.**

Both halves are externally checked.  The first half is scored against
JSONTestSuite, which we did not write: every `y_*` file accepted, every `n_*`
file rejected, and a stated, consistent answer for every `i_*` file.  Its second half is checked by parsing every truncation of every corpus file under
`-fsanitize=address,undefined`: 70k-odd parses of deliberately malformed
input, and by a depth limit that turns a 100000-deep document into an error
return instead of a stack overflow.

A JSON parser that claims conformance without a number from an external corpus
is making an unverifiable claim.  The number is the deliverable.

## Oracle

Ranked by how little we control them.

1. JSONTestSuite (github.com/nst/JSONTestSuite, MIT, Nicolas Seriot).  318
   files in `test_parsing/`: 95 `y_` must-accept, 188 `n_` must-reject, 35 `i_`
   implementation-defined.  Not ours, not derived from our reading of the spec,
   and specifically built out of the inputs that broke real parsers.  Vendored
   as a 12KB tarball under `t/` so `make test` needs no network, with its
   licence beside it.
2. CPython's `json` module.  A differential oracle in two directions:
   accept/reject agreement over the whole corpus plus generated documents, and
   value agreement, parse with both, canonicalise both, compare the text.
   Independent implementation, independent authors, and it disagrees with us in
   places we then have to justify (it accepts `NaN`, `Infinity` and lone
   surrogates. We do not).
3. The sanitizers.  `-fsanitize=address,undefined` over the truncation
   sweep.  Not a correctness oracle, but the only authority on memory safety
   that is not our own opinion, and the truncation sweep is the input shape
   designed to provoke it.
4. Round-trip.  parse -> serialise -> parse must give the same tree, and
   the second serialisation must be byte-identical to the first.  Self-
   consistency only, so it is ranked last, but it is the only check that
   exercises the serialiser against the parser.

## Non-goals

- Comments, trailing commas, unquoted keys, single-quoted strings, `NaN`,
  `Infinity`, hex numbers, leading `+`, leading zeros.  All rejected.  "Strict"
  is the entire point. A lenient mode would halve the value of the score.
- Preserving integer precision.  RFC 8259 section 6 permits the double-only
  interpretation and says so; `json_number` is a `double` and
  `123456789012345678901` loses bits.  Stated in the README, not fixed.
- Streaming or incremental parsing.  The whole document is in memory anyway
  because the DOM is.
- Sorting object keys or deduplicating them.  Duplicates are kept in document
  order and `json_get` returns the last match.

## Commit sequence

- [x] `the lexer: whitespace, literals, error offsets, CLI skeleton`
      why: an accept/reject harness has to exist before anything can be scored
      against it, and `null`/`true`/`false` is enough grammar to test the
      framing, the whitespace set and the trailing-garbage rule.
      watched failing: isspace(3) in place of the four RFC ws bytes, 2 failures.
- [x] `strings: the escape set, bare control characters, UTF-8 validation`
      why: strings are where the corpus concentrates its failures, and the
      decoder has to be right before surrogates make sense.
- [x] `surrogate pairs, and lone surrogates rejected`
      why: the single most common real-parser bug in the suite.
- [x] `numbers: the RFC grammar by hand, strtod only for the value`
      why: strtod accepts 0x10, inf, nan, leading + and whitespace, and on
      "1e" silently consumes just the "1", so it cannot be the validator.
- [x] `arrays`
      why: first recursive production.
- [x] `objects, duplicate keys kept in document order`
      why: second recursive production, and the duplicate-key policy had to be
      decided here, not discovered later.
- [x] `the depth limit: a 1000000-deep document is an error, not a SIGSEGV`
      why: the corpus ships a 100000-deep document to crash recursive-descent
      parsers, and a crash is not a rejection.
      watched failing: -DJSONC_MAX_DEPTH=100000000 gives exit 139.
- [x] `the serialiser: compact canonical form, shortest round-tripping numbers`
      why: %.17g round-trips and prints 0.1 as 0.10000000000000001; the
      shortest spelling that round-trips is what a differential oracle can
      compare as text.
- [x] `the pretty printer and the CLI flags`
      why: -p output is byte-comparable with json.dumps(indent=2), so the
      pretty printer is oracle-checked, not eyeballed.  Found the
      signed-zero divergence immediately.
- [x] `the conformance runner over vendored JSONTestSuite: 95/95, 188/188`
      why: this is the project.  Everything before it was unverified.
      watched failing: a trailing comma allowed in parse_array, 186/188.
- [x] `the truncation sweep, and the harness bug that made it blind`
      why: every prefix of every corpus file is 12605 malformed documents we
      did not have to invent.
      watched failing: hex4's length check removed produced a CLEAN sanitizer
      run until slurp stopped over-allocating.  The real bug of the project.
- [x] `the CPython differential oracle: 23 pinned divergences`
      why: a second independent implementation, over the corpus, 120 generated
      documents and a named edge battery.
      watched failing: write_number pinned to 17 digits, 5 value mismatches.
- [x] `t/unit.c: the API the CLI cannot reach`
      why: the CLI answers one bit, so error codes, byte offsets, embedded
      NUL, duplicate-key lookup and json_write are invisible from a shell.
      found: json_parse(NULL, 0) computed NULL + 0, reported by UBSan.
- [x] `make test-valgrind: a second memory oracle, and no silent skips`
      why: the target set JSONC_VALGRIND=1 and t/run.sh never read it, so for
      thirteen commits it rebuilt and then ran the ordinary suite.
- [x] `error messages: line and column, with the byte offset in brackets`
      why: "file:1000: nesting too deep" reads as a line number and is a byte
      offset.  Also records the unrepro reproducibility verdict.
- [x] `README: publish the score, the 23 divergences, and what is broken`
      why: a README that only lists successes is not finished.
- [x] `PLAN: record the final state`

Next, in order:

- [ ] `an arena allocator behind json_parse`
      why: one calloc per node means a 100k-element array is 100k small
      allocations and json_free is 100k frees.  An arena makes the free path a
      single call, which also deletes every partial-free path in the error
      handling: the place where a leak would hide.  Measure first: nothing
      here is currently performance-bound, so this is only worth doing if
      `-S` on the corpus is dominated by malloc, which has not been checked.
- [ ] `locale independence, or an explicit refusal`
      why: the one limitation in the README that is asserted from the
      spec, not demonstrated, because no comma-decimal locale is installed.
      Either install one and add the test, or parse and print the decimal
      digits directly and stop calling strtod and snprintf at all.  The second
      is more code and removes the last thing that can surprise a library
      caller.
- [ ] `a tree-building API, or a documented decision not to have one`
      why: the serialiser currently exists only to prove the parser
      round-trips.  Half an API (read but not write) is a thing someone will
      file a bug about, and "no" in the README is a cheaper answer than
      json_set.
- [ ] `coverage-guided fuzzing`
      why: /dev/urandom plus the truncation sweep is the cheap version.  gcc
      has -fsanitize=fuzzer-no-link only via clang's libFuzzer, and afl is not
      installed, so this needs a decision about what is allowed on the machine
      before it needs code.
- [ ] `score the suite's other corpora`
      why: only test_parsing/ is vendored.  test_transform/ asks what a parser
      does with documents whose *meaning* is ambiguous (duplicate keys, huge
      numbers), exactly where our policies are stated but unverified
      against anyone else's expectations.

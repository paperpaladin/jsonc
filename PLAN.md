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
file rejected, and a stated, consistent answer for every `i_*` file.  The
second half is checked by parsing every truncation of every corpus file under
`-fsanitize=address,undefined` -- 70k-odd parses of deliberately malformed
input -- and by a depth limit that turns a 100000-deep document into an error
return instead of a stack overflow.

A JSON parser that claims conformance without a number from an external corpus
is making an unverifiable claim.  The number is the deliverable.

## Oracle

Ranked by how little we control them.

1. **JSONTestSuite** (github.com/nst/JSONTestSuite, MIT, Nicolas Seriot).  318
   files in `test_parsing/`: 95 `y_` must-accept, 188 `n_` must-reject, 35 `i_`
   implementation-defined.  Not ours, not derived from our reading of the spec,
   and specifically built out of the inputs that broke real parsers.  Vendored
   as a 12KB tarball under `t/` so `make test` needs no network, with its
   licence beside it.
2. **CPython's `json` module.**  A differential oracle in two directions:
   accept/reject agreement over the whole corpus plus generated documents, and
   value agreement -- parse with both, canonicalise both, compare the text.
   Independent implementation, independent authors, and it disagrees with us in
   places we then have to justify (it accepts `NaN`, `Infinity` and lone
   surrogates; we do not).
3. **The sanitizers.**  `-fsanitize=address,undefined` over the truncation
   sweep.  Not a correctness oracle, but the only authority on memory safety
   that is not our own opinion, and the truncation sweep is the input shape
   designed to provoke it.
4. **Round-trip.**  parse -> serialise -> parse must give the same tree, and
   the second serialisation must be byte-identical to the first.  Self-
   consistency only, so it is ranked last, but it is the only check that
   exercises the serialiser against the parser.

## Non-goals

- Comments, trailing commas, unquoted keys, single-quoted strings, `NaN`,
  `Infinity`, hex numbers, leading `+`, leading zeros.  All rejected.  "Strict"
  is the entire point; a lenient mode would halve the value of the score.
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
- [ ] `strings: the escape set, bare control characters, UTF-8 validation`
      why: strings are where the corpus concentrates its failures (90 of the
      318 files), and the decoder has to be right before surrogates make sense.
- [ ] `surrogate pairs, and lone surrogates rejected`
      why: the single most common real-parser bug in the suite; needs the
      escape decoder in place first.
- [ ] `numbers: the RFC grammar by hand, strtod for the value`
      why: `strtod` accepts `0x10`, `inf`, `nan`, leading `+` and whitespace,
      so it cannot be the validator -- only the evaluator.
- [ ] `arrays`
      why: first recursive production; needs a value parser to recurse into.
- [ ] `objects, duplicate keys kept in order`
      why: second recursive production, and the duplicate-key policy has to be
      decided here rather than discovered later.
- [ ] `the depth limit`
      why: the corpus contains a 100000-deep document specifically to crash
      recursive-descent parsers, and a crash is not a rejection.
- [ ] `the serialiser: compact, with shortest round-tripping numbers`
      why: `%.17g` round-trips but prints `0.10000000000000001`; the shortest
      form that still round-trips is what the differential oracle can match.
- [ ] `the pretty printer and the CLI flags`
      why: `-p` output is byte-comparable with `json.dumps(indent=2)`, which
      makes the pretty printer oracle-checked rather than eyeballed.
- [ ] `the conformance runner over vendored JSONTestSuite`
      why: this is the project.  Everything before it is unverified.
- [ ] one commit per class of failure the corpus finds
      why: each is a real bug with a named external file as its reproducer.
- [ ] `the truncation sweep`
      why: every prefix of every corpus file is 70k malformed documents we did
      not have to invent, and it is what `make test-asan` is for.
- [ ] `the CPython differential oracle`
      why: a second independent implementation, over generated documents and
      the edge cases the corpus does not cover (very long strings, deep but
      legal nesting, doubles at the edge of the format).
- [ ] `README and PLAN: publish the score, including the failures`
      why: a README that lists only successes is not finished.

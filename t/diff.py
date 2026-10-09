#!/usr/bin/env python3
# t/diff.py -- differential oracle: jsonc against CPython's json module.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Three comparisons, all against an implementation we did not write:
#
#   1. accept/reject over the whole JSONTestSuite corpus.  Where the two
#      disagree, the disagreement is pinned in t/py_divergences.expected, so a
#      new one fails the suite and a documented one does not.
#   2. value agreement.  Parse the same document with both, canonicalise both
#      to text under one shared rule, compare the text.  This is the only check
#      that looks at what was parsed rather than whether it parsed.
#   3. generated documents, both directions: CPython writes, jsonc reads and
#      rewrites, CPython reads that back.
#
# Prints "ok - " / "not ok - " lines for t/run.sh to tally.

import json
import os
import random
import struct
import subprocess
import sys

JSONC = sys.argv[1] if len(sys.argv) > 1 else "./jsonc"
CORPUS = sys.argv[2] if len(sys.argv) > 2 else "./scratch/test_parsing"
EXPECTED = "t/py_divergences.expected"

# canon() recurses two frames per nesting level and the corpus contains a
# legal 500-deep array, so the default limit of 1000 is not enough.  CPython's
# own json scanner has a separate, lower effective ceiling, which is why the
# battery below stops at 400 rather than at jsonc's 1000.
sys.setrecursionlimit(20000)

nfail = 0


def ok(msg):
    print("ok - " + msg)


def nok(msg):
    global nfail
    nfail += 1
    print("not ok - " + msg)


def jsonc(data, *flags):
    """Returns (exit status, stdout bytes)."""
    p = subprocess.run([JSONC, *flags], input=data,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return p.returncode, p.stdout


class Obj(list):
    """An object as an ordered list of (key, value) pairs.

    json.loads(object_pairs_hook=...) is what makes CPython lossless about
    duplicate keys, which jsonc also is.  Without it {"a":1,"a":2} collapses to
    one member on the Python side only and every duplicate-key document would
    have to be skipped instead of compared.
    """


def py_load(data):
    """CPython's answer for these bytes: (accepted?, value).

    Deliberately json.loads(bytes), not json.loads(str): the bytes form is what
    CPython documents and it sniffs UTF-16/UTF-32 BOMs, which is one of the
    pinned divergences below.
    """
    try:
        return True, json.loads(data, object_pairs_hook=Obj)
    except Exception:
        return False, None


# --- the shared canonical form ---------------------------------------------
# Identical by construction to write_number()/write_string() in jsonc.c: of
# "%.1g" through "%.17g", the shortest spelling that reads back equal.

def fmt(x):
    best = None
    for p in range(1, 18):
        c = "%.*g" % (p, x)
        if float(c) == x and (best is None or len(c) < len(best)):
            best = c
    return best


def jstr(s):
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif o < 0x20:
            out.append("\\u%04x" % o)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def canon(v):
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, str):
        return jstr(v)
    if isinstance(v, Obj):
        return "{" + ",".join(jstr(k) + ":" + canon(x) for k, x in v) + "}"
    if isinstance(v, dict):
        return "{" + ",".join(jstr(k) + ":" + canon(x) for k, x in v.items()) + "}"
    if isinstance(v, list):
        return "[" + ",".join(canon(x) for x in v) + "]"
    if isinstance(v, int):
        return fmt(float(v))      # OverflowError here means jsonc rejects it
    if isinstance(v, float):
        return fmt(v)
    raise TypeError(repr(v))


# --- 1. accept/reject over the corpus --------------------------------------

def corpus_verdicts():
    if not os.path.isdir(CORPUS):
        nok("corpus directory %s present" % CORPUS)
        return []
    files = sorted(f for f in os.listdir(CORPUS) if f.endswith(".json"))
    rows, agree, both = [], 0, []
    for name in files:
        data = open(os.path.join(CORPUS, name), "rb").read()
        mine = jsonc(data)[0] == 0
        theirs = py_load(data)[0]
        if mine == theirs:
            agree += 1
        else:
            rows.append("%s\t%s\n" % (name,
                        "jsonc accepts, CPython rejects" if mine else
                        "CPython accepts, jsonc rejects"))
        if mine and theirs:
            both.append(name)
    got = "".join(rows)
    print("DIFF accept/reject agreement with CPython json: %d/%d files"
          % (agree, len(files)))
    try:
        want = open(EXPECTED).read()
    except OSError:
        want = None
    if want is None:
        nok("%s missing; current divergences:\n%s" % (EXPECTED, got))
    elif want == got:
        ok("corpus divergences from CPython match %s (%d of them)"
           % (EXPECTED, len(rows)))
    else:
        nok("corpus divergences changed")
        print("--- expected\n%s--- got\n%s" % (want, got))
    return both


# --- 2. value agreement on everything both accept --------------------------

# The two corpus files where a value mismatch is expected rather than a bug:
# CPython reads "-0" as the integer 0 and loses the sign, we read every number
# as a double and keep it.  Narrowed to these two names on purpose -- a blanket
# "ignore signed zero" rule would hide a real sign bug anywhere else.
SIGNED_ZERO = {"y_number_minus_zero.json", "y_number_negative_zero.json"}


def corpus_values(both):
    bad = 0
    excused = 0
    for name in both:
        data = open(os.path.join(CORPUS, name), "rb").read()
        st, out = jsonc(data, "-c")
        if st != 0:
            nok("-c on %s exited %d" % (name, st))
            bad += 1
            continue
        try:
            want = canon(py_load(data)[1])
        except OverflowError:
            continue          # a Python int too large for a double
        got = out.decode("utf-8").rstrip("\n")
        if got != want:
            if name in SIGNED_ZERO and got.replace("-0", "0") == want:
                excused += 1
                continue
            nok("value mismatch on %s: jsonc %r, CPython %r" % (name, got, want))
            bad += 1
    if not bad:
        ok("canonical form agrees with CPython on %d of %d mutually accepted "
           "corpus files (%d excused: signed zero)"
           % (len(both) - excused, len(both), excused))
    if excused != len(SIGNED_ZERO):
        nok("expected %d signed-zero divergences, saw %d"
            % (len(SIGNED_ZERO), excused))


# --- 3. generated documents ------------------------------------------------

EDGE_NUMBERS = [
    0.0, 1.0, -1.0, 0.5, 1 / 3, 1e-300, 1e300,
    1.7976931348623157e308, -1.7976931348623157e308,
    2.2250738585072014e-308, 5e-324, -5e-324,
    9007199254740992.0, 9007199254740993.0, 1e15, 1e16, 1e17,
    123456789.123456789, 2 ** 53, -(2 ** 53), 10 ** 20, -(10 ** 25),
]

ESCAPE_STRINGS = [
    "", "a", "\\", '"', "/", "\b\f\n\r\t",
    "\x00", "\x01\x1f", "\x7f",
    "\u00e9", "\u07ff", "\u0800", "\uffff", "\ufffd",
    "\U00010000", "\U0001f600", "\U0010ffff",
    "a" * 70000,
    "\U0001f600" * 5000,
    "line1\nline2\ttab\\slash\"quote",
]


def gen(rng, depth):
    pick = rng.randrange(10)
    if depth <= 0 or pick < 3:
        leaf = rng.randrange(6)
        if leaf == 0:
            return None
        if leaf == 1:
            return rng.choice([True, False])
        if leaf == 2:
            return rng.choice(EDGE_NUMBERS)
        if leaf == 3:
            # A random bit pattern is the only way to hit mantissas no
            # human would write.  NaN and both infinities are excluded:
            # jsonc has no spelling for them and CPython's is not JSON.
            while True:
                d = struct.unpack("<d", rng.randbytes(8))[0]
                if d == d and abs(d) != float("inf"):
                    return d
        if leaf == 4:
            return rng.choice(ESCAPE_STRINGS)
        n = rng.randrange(20)
        return "".join(chr(rng.choice([rng.randrange(0x20, 0xd800),
                                       rng.randrange(0xe000, 0x110000)]))
                       for _ in range(n))
    if pick < 7:
        return [gen(rng, depth - 1) for _ in range(rng.randrange(5))]
    return {("k%d" % i): gen(rng, depth - 1) for i in range(rng.randrange(5))}


def generated(n=120):
    rng = random.Random(20261009)
    bad = 0
    for i in range(n):
        obj = gen(rng, rng.randrange(1, 6))
        try:
            want = canon(obj)
        except OverflowError:
            continue
        text = json.dumps(obj, separators=(",", ":"),
                          ensure_ascii=bool(i & 1)).encode("utf-8")
        st, out = jsonc(text, "-c")
        if st != 0:
            nok("generated #%d: jsonc rejected CPython's own output" % i)
            bad += 1
            continue
        got = out.decode("utf-8").rstrip("\n")
        if got != want:
            nok("generated #%d: jsonc %r != canon %r" % (i, got[:120], want[:120]))
            bad += 1
            continue
        # ... and CPython must read our output back to the same value.
        back = py_load(out)
        if not back[0]:
            nok("generated #%d: CPython rejected jsonc's output" % i)
            bad += 1
            continue
        if canon(back[1]) != want:
            nok("generated #%d: CPython read jsonc's output differently" % i)
            bad += 1
            continue
        # ... and the pretty form must mean the same as the compact one.
        stp, pretty = jsonc(text, "-p")
        if stp != 0 or jsonc(pretty, "-c")[1] != out:
            nok("generated #%d: -p output does not re-read as -c output" % i)
            bad += 1
    if not bad:
        ok("%d generated documents round-trip through jsonc and CPython "
           "identically" % n)


# --- 4. the explicit edge battery ------------------------------------------
# Shapes worth naming, as literal documents.  Deep but legal nesting is here
# rather than in the generator because CPython's own recursion limit is in
# play: 400 is comfortable for both, and jsonc's own ceiling is tested in
# t/run.sh where CPython is not involved.

def battery():
    docs = [
        ("empty containers", b'[[],{},[{}],{"a":[]}]'),
        ("deep legal nesting (400)", b"[" * 400 + b"1" + b"]" * 400),
        ("deep legal objects (400)", b'{"a":' * 400 + b"1" + b"}" * 400),
        ("all escape forms", b'"\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u001f\\u007f\\uffff"'),
        ("surrogate pairs", b'"\\ud800\\udc00\\udbff\\udfff\\ud83d\\ude00"'),
        ("raw UTF-8 at every length",
         (chr(34) + chr(65) + chr(0xe9) + chr(0x800) + chr(0x1f600) + chr(34)).encode()),
        ("subnormals and signed zero", b"[5e-324,-5e-324,2.2250738585072011e-308,0.0]"),
        ("integers either side of 2^53", b"[9007199254740992,9007199254740993,-9007199254740993]"),
        ("number at DBL_MAX", b"[1.7976931348623157e308,-1.7976931348623157e308]"),
        ("very long string (1MB)", b'"' + b"x" * (1 << 20) + b'"'),
        ("very long array (100k)", b"[" + b",".join(b"1" for _ in range(100000)) + b"]"),
        ("many keys (10k)", b"{" + b",".join(
            b'"k%d":%d' % (i, i) for i in range(10000)) + b"}"),
        ("all of 0x20..0x10FFFF in one string, sampled",
         json.dumps("".join(chr(c) for c in range(0x20, 0xd800, 97)) +
                    "".join(chr(c) for c in range(0xe000, 0x110000, 997))).encode()),
    ]
    for label, data in docs:
        st, out = jsonc(data, "-c")
        accepted, value = py_load(data)
        if st != 0 or not accepted:
            nok("battery %s: jsonc exit %d, CPython accepted %s"
                % (label, st, accepted))
            continue
        try:
            want = canon(value)
        except OverflowError:
            ok("battery %s: both accept (value too large for a double to "
               "compare)" % label)
            continue
        got = out.decode("utf-8").rstrip("\n")
        if got == want:
            ok("battery %s: identical canonical form" % label)
        else:
            nok("battery %s: jsonc %r != CPython %r"
                % (label, got[:100], want[:100]))

    # Duplicate keys: both must accept, and jsonc must keep both members.
    st, out = jsonc(b'{"a":1,"a":2,"b":3,"a":4}', "-c")
    if st == 0 and out.strip() == b'{"a":1,"a":2,"b":3,"a":4}':
        ok("battery duplicate keys: all four members kept in order")
    else:
        nok("battery duplicate keys: got %r" % out)
    if py_load(b'{"a":1,"a":2}')[0]:
        ok("battery duplicate keys: CPython accepts them too (it keeps the last)")
    else:
        nok("battery duplicate keys: CPython rejected them")


both = corpus_verdicts()
corpus_values(both)
generated()
battery()
sys.exit(1 if nfail else 0)

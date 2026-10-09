/* jsonc -- a strict RFC 8259 JSON parser, serialiser and CLI in one C11 file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The whole thing is one translation unit on purpose: there is no header, and
 * the test programs in t/ `#include "../jsonc.c"` with JSONC_NO_MAIN defined.
 * That is one file instead of three and nothing to keep in sync.
 *
 * Strictness is the contract.  RFC 8259 and nothing else: no comments, no
 * trailing commas, no NaN/Infinity, no single quotes, no unquoted keys, no
 * leading '+', no leading zeros, no bare control characters in strings, no
 * lone surrogates, no ill-formed UTF-8.  Every rejection carries a byte
 * offset.  The parser never reads past the buffer it was handed and never
 * recurses deeper than JSONC_MAX_DEPTH.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recursion ceiling.  Hit it and parsing fails with JSON_ERR_DEPTH; the point
 * is that a 100000-deep document is an error return, never a stack overflow.
 * 1000 frames of parse_value is well under a default 8MB stack even with the
 * fatter frames that -fsanitize=address generates. */
#ifndef JSONC_MAX_DEPTH
#define JSONC_MAX_DEPTH 1000
#endif

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} json_type;

typedef struct json json;

typedef struct {
	char *key;		/* decoded UTF-8, NUL-terminated and length-counted */
	size_t klen;
	json *val;
} json_member;

struct json {
	json_type type;
	union {
		int boolean;
		double number;
		struct { char *p; size_t len; } string;
		struct { json **v; size_t len, cap; } array;
		struct { json_member *v; size_t len, cap; } object;
	} u;
};

typedef enum {
	JSON_OK = 0,
	JSON_ERR_EOF,		/* input ended mid-value */
	JSON_ERR_TOKEN,		/* unexpected byte where a value was expected */
	JSON_ERR_TRAILING,	/* junk after the top-level value */
	JSON_ERR_NUMBER,	/* malformed number per the RFC grammar */
	JSON_ERR_RANGE,		/* number overflows a double to +-infinity */
	JSON_ERR_STRING,	/* bad escape, bare control char, bad UTF-8 */
	JSON_ERR_SURROGATE,	/* unpaired \uD800-\uDFFF */
	JSON_ERR_UTF8,		/* ill-formed UTF-8 in a string */
	JSON_ERR_COMMA,		/* missing or stray ',' / ':' in a container */
	JSON_ERR_DEPTH,		/* nesting exceeded JSONC_MAX_DEPTH */
	JSON_ERR_NOMEM
} json_err;

static const char *const json_err_msg[] = {
	"ok",
	"unexpected end of input",
	"unexpected token",
	"trailing garbage after value",
	"malformed number",
	"number out of range for double",
	"malformed string",
	"unpaired surrogate escape",
	"ill-formed UTF-8",
	"expected ',' or ':'",
	"nesting too deep",
	"out of memory"
};

const char *json_strerror(json_err e)
{
	return json_err_msg[(unsigned)e <= JSON_ERR_NOMEM ? (unsigned)e : 0];
}

/* ---------------------------------------------------------------- parser --- */

typedef struct {
	const unsigned char *b;	/* base, for offset arithmetic */
	const unsigned char *p;
	const unsigned char *end;
	int depth;
	json_err err;
	size_t errpos;
} P;

/* Record the first error only: the offset of the byte that broke the parse is
 * more useful than the offset where the unwinding noticed. */
static int fail(P *x, json_err e, const unsigned char *at)
{
	if (x->err == JSON_OK) {
		x->err = e;
		x->errpos = (size_t)(at - x->b);
	}
	return -1;
}

static json *node(P *x, json_type t)
{
	json *j = calloc(1, sizeof *j);
	if (!j) {
		fail(x, JSON_ERR_NOMEM, x->p);
		return NULL;
	}
	j->type = t;
	return j;
}

void json_free(json *j)
{
	size_t i;
	if (!j)
		return;
	switch (j->type) {
	case JSON_STRING:
		free(j->u.string.p);
		break;
	case JSON_ARRAY:
		for (i = 0; i < j->u.array.len; i++)
			json_free(j->u.array.v[i]);
		free(j->u.array.v);
		break;
	case JSON_OBJECT:
		for (i = 0; i < j->u.object.len; i++) {
			free(j->u.object.v[i].key);
			json_free(j->u.object.v[i].val);
		}
		free(j->u.object.v);
		break;
	default:
		break;
	}
	free(j);
}

/* RFC 8259 section 2: ws = *( %x20 / %x09 / %x0A / %x0D ).  Nothing else, so a
 * vertical tab or a form feed between tokens is a syntax error. */
static void skip_ws(P *x)
{
	while (x->p < x->end) {
		unsigned char c = *x->p;
		if (c == 0x20 || c == 0x09 || c == 0x0a || c == 0x0d)
			x->p++;
		else
			break;
	}
}

static int lit(P *x, const char *s, size_t n)
{
	if ((size_t)(x->end - x->p) < n)
		return fail(x, JSON_ERR_EOF, x->end);
	if (memcmp(x->p, s, n) != 0)
		return fail(x, JSON_ERR_TOKEN, x->p);
	x->p += n;
	return 0;
}


/* A growable byte buffer for decoded strings.  Always NUL-terminated for the
 * convenience of callers that know the string has no embedded NUL; `len` is
 * the authority, because "" is a legal JSON string and decodes to one
 * NUL byte. */
typedef struct { char *p; size_t len, cap; } buf;

static int buf_room(P *x, buf *b, size_t n)
{
	size_t want = b->len + n + 1;	/* +1 for the terminator */
	char *q;
	if (want <= b->cap)
		return 0;
	while (b->cap < want)
		b->cap = b->cap ? b->cap * 2 : 16;
	if (!(q = realloc(b->p, b->cap)))
		return fail(x, JSON_ERR_NOMEM, x->p);
	b->p = q;
	return 0;
}

static int buf_put(P *x, buf *b, const void *s, size_t n)
{
	if (buf_room(x, b, n) < 0)
		return -1;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
	return 0;
}

static int buf_byte(P *x, buf *b, unsigned char c)
{
	return buf_put(x, b, &c, 1);
}

/* Copy one UTF-8 sequence starting at x->p.  This is the whole of RFC 3629
 * written out: the second byte's legal range depends on the lead byte, which
 * is what rules out overlong encodings (0xC0/0xC1, 0xE0 0x80), the UTF-16
 * surrogate range encoded directly (0xED 0xA0), and anything above U+10FFFF
 * (0xF4 0x90, 0xF5-0xFF).  A range check on the lead byte alone accepts all
 * three, which is the classic hole. */
static int copy_utf8(P *x, buf *b)
{
	const unsigned char *p = x->p;
	unsigned char c = p[0];
	size_t n, i;
	unsigned lo = 0x80, hi = 0xbf;

	if (c >= 0xc2 && c <= 0xdf)		n = 2;
	else if (c == 0xe0)			{ n = 3; lo = 0xa0; }
	else if (c == 0xed)			{ n = 3; hi = 0x9f; }
	else if (c >= 0xe1 && c <= 0xef)	n = 3;
	else if (c == 0xf0)			{ n = 4; lo = 0x90; }
	else if (c >= 0xf1 && c <= 0xf3)	n = 4;
	else if (c == 0xf4)			{ n = 4; hi = 0x8f; }
	else					return fail(x, JSON_ERR_UTF8, p);

	if ((size_t)(x->end - p) < n)
		return fail(x, JSON_ERR_UTF8, x->end);
	for (i = 1; i < n; i++) {
		unsigned char t = p[i];
		if (t < (i == 1 ? lo : 0x80) || t > (i == 1 ? hi : 0xbf))
			return fail(x, JSON_ERR_UTF8, p + i);
	}
	if (buf_put(x, b, p, n) < 0)
		return -1;
	x->p = p + n;
	return 0;
}

static int encode_utf8(P *x, buf *b, unsigned cp)
{
	unsigned char o[4];
	if (cp < 0x80)
		return buf_byte(x, b, (unsigned char)cp);
	if (cp < 0x800) {
		o[0] = (unsigned char)(0xc0 | (cp >> 6));
		o[1] = (unsigned char)(0x80 | (cp & 0x3f));
		return buf_put(x, b, o, 2);
	}
	if (cp < 0x10000) {
		o[0] = (unsigned char)(0xe0 | (cp >> 12));
		o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
		o[2] = (unsigned char)(0x80 | (cp & 0x3f));
		return buf_put(x, b, o, 3);
	}
	o[0] = (unsigned char)(0xf0 | (cp >> 18));
	o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
	o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
	o[3] = (unsigned char)(0x80 | (cp & 0x3f));
	return buf_put(x, b, o, 4);
}

/* Four hex digits at x->p, advancing past them.  strtoul is not usable here:
 * it skips leading whitespace, accepts a sign, and would read past the end of
 * a buffer that is not NUL-terminated. */
static int hex4(P *x, unsigned *out)
{
	unsigned v = 0;
	int i;
	if ((size_t)(x->end - x->p) < 4)
		return fail(x, JSON_ERR_EOF, x->end);
	for (i = 0; i < 4; i++) {
		unsigned char c = x->p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')	v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')	v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')	v |= (unsigned)(c - 'A' + 10);
		else				return fail(x, JSON_ERR_STRING, x->p + i);
	}
	x->p += 4;
	*out = v;
	return 0;
}

/* On failure the buffer is freed and zeroed, so a caller that only checks the
 * return value cannot leak. */
static int parse_string(P *x, buf *b)
{
	b->p = NULL;
	b->len = b->cap = 0;

	if (x->p >= x->end || *x->p != '"') {
		fail(x, x->p >= x->end ? JSON_ERR_EOF : JSON_ERR_TOKEN,
		     x->p >= x->end ? x->end : x->p);
		return -1;
	}
	x->p++;
	for (;;) {
		unsigned char c;
		if (x->p >= x->end) {
			fail(x, JSON_ERR_EOF, x->end);
			goto bad;
		}
		c = *x->p;
		if (c == '"') {
			x->p++;
			break;
		}
		if (c == '\\') {
			const unsigned char *esc = x->p;
			unsigned cp;
			if (x->end - x->p < 2) {
				fail(x, JSON_ERR_EOF, x->end);
				goto bad;
			}
			x->p += 2;
			switch (esc[1]) {
			case '"':  if (buf_byte(x, b, '"')  < 0) goto bad; break;
			case '\\': if (buf_byte(x, b, '\\') < 0) goto bad; break;
			case '/':  if (buf_byte(x, b, '/')  < 0) goto bad; break;
			case 'b':  if (buf_byte(x, b, '\b') < 0) goto bad; break;
			case 'f':  if (buf_byte(x, b, '\f') < 0) goto bad; break;
			case 'n':  if (buf_byte(x, b, '\n') < 0) goto bad; break;
			case 'r':  if (buf_byte(x, b, '\r') < 0) goto bad; break;
			case 't':  if (buf_byte(x, b, '\t') < 0) goto bad; break;
			case 'u':
				if (hex4(x, &cp) < 0)
					goto bad;
				/* UTF-16 leaks into JSON here and nowhere else: a
				 * codepoint above U+FFFF can only be written as a
				 * surrogate pair, so a high surrogate *must* be
				 * followed by \uDC00-\uDFFF and a low surrogate can
				 * never stand alone.  Anything else is an unpaired
				 * surrogate, which is not a Unicode scalar value and
				 * therefore cannot be encoded as UTF-8 at all.  The
				 * popular wrong answers are to emit CESU-8, to emit
				 * U+FFFD, or to emit the raw 0xED 0xA0 0x80 form that
				 * copy_utf8() above refuses to read back. */
				if (cp >= 0xdc00 && cp <= 0xdfff) {
					fail(x, JSON_ERR_SURROGATE, esc);
					goto bad;
				}
				if (cp >= 0xd800 && cp <= 0xdbff) {
					unsigned lo;
					if ((size_t)(x->end - x->p) < 6 ||
					    x->p[0] != '\\' || x->p[1] != 'u') {
						fail(x, JSON_ERR_SURROGATE, esc);
						goto bad;
					}
					x->p += 2;
					if (hex4(x, &lo) < 0)
						goto bad;
					if (lo < 0xdc00 || lo > 0xdfff) {
						fail(x, JSON_ERR_SURROGATE, esc);
						goto bad;
					}
					cp = 0x10000 + ((cp - 0xd800) << 10) +
					     (lo - 0xdc00);
				}
				if (encode_utf8(x, b, cp) < 0)
					goto bad;
				break;
			default:
				fail(x, JSON_ERR_STRING, esc + 1);
				goto bad;
			}
			continue;
		}
		/* RFC 8259 section 7: unescaped = %x20-21 / %x23-5B / %x5D-10FFFF.
		 * Everything below 0x20 must be escaped; DEL need not be. */
		if (c < 0x20) {
			fail(x, JSON_ERR_STRING, x->p);
			goto bad;
		}
		if (c < 0x80) {
			if (buf_byte(x, b, c) < 0)
				goto bad;
			x->p++;
			continue;
		}
		if (copy_utf8(x, b) < 0)
			goto bad;
	}
	if (buf_room(x, b, 0) < 0)	/* guarantees p != NULL for "" */
		goto bad;
	b->p[b->len] = 0;
	return 0;
bad:
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
	return -1;
}


#define DIGIT(c) ((c) >= '0' && (c) <= '9')

/* RFC 8259 section 6:
 *   number = [ "-" ] int [ frac ] [ exp ]
 *   int    = "0" / ( %x31-39 *DIGIT )
 *   frac   = "." 1*DIGIT
 *   exp    = ("e"/"E") [ "-"/"+" ] 1*DIGIT
 *
 * Validated by hand, then evaluated with strtod.  strtod cannot be the
 * validator: it accepts "0x10", "inf", "nan", "+1", " 1", "1.", ".1" and
 * "1e" (consuming just the "1"), all of which this function rejects.  It is
 * only ever asked to convert a token this function has already proved legal.
 */
static int parse_number(P *x, double *out)
{
	const unsigned char *s = x->p, *p = s;
	char tmp[64], *dyn = NULL, *txt, *endp;
	size_t n;
	double d;

	if (p < x->end && *p == '-')
		p++;
	if (p >= x->end)
		return fail(x, JSON_ERR_EOF, x->end);
	if (*p == '0') {
		p++;
		/* "01", "00", "-01": the int production is "0" or [1-9]*DIGIT,
		 * so a digit after a leading zero is not a longer number, it is
		 * a syntax error.  Reporting it here rather than letting it fall
		 * out as trailing garbage is the difference between "malformed
		 * number at offset 1" and "trailing garbage at offset 1". */
		if (p < x->end && DIGIT(*p))
			return fail(x, JSON_ERR_NUMBER, p);
	} else if (DIGIT(*p)) {
		while (p < x->end && DIGIT(*p))
			p++;
	} else {
		return fail(x, JSON_ERR_NUMBER, p);
	}
	if (p < x->end && *p == '.') {
		p++;
		if (p >= x->end || !DIGIT(*p))
			return fail(x, p >= x->end ? JSON_ERR_EOF : JSON_ERR_NUMBER,
				    p >= x->end ? x->end : p);
		while (p < x->end && DIGIT(*p))
			p++;
	}
	if (p < x->end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < x->end && (*p == '+' || *p == '-'))
			p++;
		if (p >= x->end || !DIGIT(*p))
			return fail(x, p >= x->end ? JSON_ERR_EOF : JSON_ERR_NUMBER,
				    p >= x->end ? x->end : p);
		while (p < x->end && DIGIT(*p))
			p++;
	}

	/* strtod needs a NUL-terminated string and the input buffer is not one.
	 * A JSON number token has no length limit (the corpus has a 137-byte
	 * one), so fall back to the heap for the long ones. */
	n = (size_t)(p - s);
	if (n < sizeof tmp) {
		txt = tmp;
	} else {
		if (!(dyn = malloc(n + 1)))
			return fail(x, JSON_ERR_NOMEM, s);
		txt = dyn;
	}
	memcpy(txt, s, n);
	txt[n] = 0;

	d = strtod(txt, &endp);
	if (endp != txt + n) {
		/* Unreachable unless the grammar check above is wrong, so it is
		 * worth an error rather than an assert: a mismatch means a token
		 * we proved legal is not legal to libc. */
		free(dyn);
		return fail(x, JSON_ERR_NUMBER, s + (endp - txt));
	}
	free(dyn);

	/* Policy: a token that overflows to +-infinity is rejected.  RFC 8259
	 * section 6 permits either answer ("such software is expected to use
	 * ... IEEE 754 binary64") and JSONTestSuite files it under i_, so the
	 * requirement is only to be consistent and say which.  Rejecting keeps
	 * the serialiser total: every tree that parses can be written back out
	 * as valid JSON, which is not true if infinities can live in it.
	 * Underflow is NOT an error: 1e-999 is 0.0 and loses only precision,
	 * the same way 123456789012345678901 does. */
	if (isinf(d))
		return fail(x, JSON_ERR_RANGE, s);

	x->p = p;
	*out = d;
	return 0;
}

static json *parse_value(P *x);

/* Takes ownership of key and val immediately, so every error path after this
 * point is just json_free() on the object. */
static int obj_push(P *x, json *j, char *key, size_t klen, json *val)
{
	if (j->u.object.len == j->u.object.cap) {
		size_t cap = j->u.object.cap ? j->u.object.cap * 2 : 8;
		json_member *q = realloc(j->u.object.v, cap * sizeof *q);
		if (!q) {
			free(key);
			json_free(val);
			return fail(x, JSON_ERR_NOMEM, x->p);
		}
		j->u.object.v = q;
		j->u.object.cap = cap;
	}
	j->u.object.v[j->u.object.len].key = key;
	j->u.object.v[j->u.object.len].klen = klen;
	j->u.object.v[j->u.object.len].val = val;
	j->u.object.len++;
	return 0;
}

/* x->p is on '{'.  A member is string ':' value and nothing else: no unquoted
 * keys, no numeric keys, no '=' instead of ':', no bare values.
 *
 * Duplicate keys: kept, all of them, in document order.  RFC 8259 section 4
 * says the names SHOULD be unique and describes implementations that take the
 * last, take the first, or report an error, so this is a choice and not a
 * conformance question -- JSONTestSuite's y_object_duplicated_key requires
 * that the document be *accepted*, which is the only part that is fixed.
 * Keeping them makes the parser lossless; json_get() below returns the last
 * match, which is what JavaScript and Python do. */
static json *parse_object(P *x)
{
	json *j = node(x, JSON_OBJECT);
	if (!j)
		return NULL;
	x->p++;
	skip_ws(x);
	if (x->p < x->end && *x->p == '}') {
		x->p++;
		return j;
	}
	for (;;) {
		buf k;
		json *v;
		skip_ws(x);
		if (parse_string(x, &k) < 0)
			goto bad;
		skip_ws(x);
		if (x->p >= x->end) {
			free(k.p);
			fail(x, JSON_ERR_EOF, x->end);
			goto bad;
		}
		if (*x->p != ':') {
			free(k.p);
			fail(x, JSON_ERR_COMMA, x->p);
			goto bad;
		}
		x->p++;
		skip_ws(x);
		if (!(v = parse_value(x))) {
			free(k.p);
			goto bad;
		}
		if (obj_push(x, j, k.p, k.len, v) < 0)
			goto bad;
		skip_ws(x);
		if (x->p >= x->end) {
			fail(x, JSON_ERR_EOF, x->end);
			goto bad;
		}
		if (*x->p == ',') {
			x->p++;
			continue;
		}
		if (*x->p == '}') {
			x->p++;
			return j;
		}
		fail(x, JSON_ERR_COMMA, x->p);
		goto bad;
	}
bad:
	json_free(j);
	return NULL;
}


/* Doubling growth.  A failed realloc leaves the old block intact and the node
 * still owns it, so the caller's json_free() cleans up either way. */
static int arr_push(P *x, json *j, json *v)
{
	if (j->u.array.len == j->u.array.cap) {
		size_t cap = j->u.array.cap ? j->u.array.cap * 2 : 8;
		json **q = realloc(j->u.array.v, cap * sizeof *q);
		if (!q) {
			json_free(v);
			return fail(x, JSON_ERR_NOMEM, x->p);
		}
		j->u.array.v = q;
		j->u.array.cap = cap;
	}
	j->u.array.v[j->u.array.len++] = v;
	return 0;
}

/* x->p is on '['.  The empty array is special-cased because "[]" is legal and
 * "[,]" and "[1,]" are not: after a comma a value is mandatory, which falls
 * out of calling parse_value() unconditionally. */
static json *parse_array(P *x)
{
	json *j = node(x, JSON_ARRAY);
	if (!j)
		return NULL;
	x->p++;
	skip_ws(x);
	if (x->p < x->end && *x->p == ']') {
		x->p++;
		return j;
	}
	for (;;) {
		json *v;
		skip_ws(x);
		if (!(v = parse_value(x)))
			goto bad;
		if (arr_push(x, j, v) < 0)
			goto bad;
		skip_ws(x);
		if (x->p >= x->end) {
			fail(x, JSON_ERR_EOF, x->end);
			goto bad;
		}
		if (*x->p == ',') {
			x->p++;
			continue;
		}
		if (*x->p == ']') {
			x->p++;
			return j;
		}
		fail(x, JSON_ERR_COMMA, x->p);
		goto bad;
	}
bad:
	json_free(j);
	return NULL;
}


static json *parse_value(P *x)
{
	json *j;
	if (x->p >= x->end) {
		fail(x, JSON_ERR_EOF, x->end);
		return NULL;
	}
	switch (*x->p) {
	case '"': {
		buf b;
		if (parse_string(x, &b) < 0)
			return NULL;
		if (!(j = node(x, JSON_STRING))) {
			free(b.p);
			return NULL;
		}
		j->u.string.p = b.p;
		j->u.string.len = b.len;
		return j;
	}
	/* The depth counter lives here rather than inside parse_array and
	 * parse_object so that there is exactly one increment, one decrement and
	 * one check for both containers.  Without it this function is a textbook
	 * stack overflow: the corpus ships a 100000-deep document whose only
	 * purpose is to crash recursive-descent parsers, and a SIGSEGV is not a
	 * rejection.  JSONC_MAX_DEPTH frames of parse_value is a few hundred
	 * kilobytes of stack even under -fsanitize=address. */
	case '[':
	case '{':
		if (x->depth >= JSONC_MAX_DEPTH) {
			fail(x, JSON_ERR_DEPTH, x->p);
			return NULL;
		}
		x->depth++;
		j = *x->p == '[' ? parse_array(x) : parse_object(x);
		x->depth--;
		return j;
	case '-': case '0': case '1': case '2': case '3':
	case '4': case '5': case '6': case '7': case '8': case '9': {
		double d;
		if (parse_number(x, &d) < 0)
			return NULL;
		if ((j = node(x, JSON_NUMBER)))
			j->u.number = d;
		return j;
	}
	case 'n':
		if (lit(x, "null", 4) < 0)
			return NULL;
		return node(x, JSON_NULL);
	case 't':
		if (lit(x, "true", 4) < 0)
			return NULL;
		if ((j = node(x, JSON_BOOL)))
			j->u.boolean = 1;
		return j;
	case 'f':
		if (lit(x, "false", 5) < 0)
			return NULL;
		if ((j = node(x, JSON_BOOL)))
			j->u.boolean = 0;
		return j;
	default:
		fail(x, JSON_ERR_TOKEN, x->p);
		return NULL;
	}
}

/* The only entry point.  `len` bytes at `buf`; embedded NUL is just another
 * byte that is not valid JSON outside a string.  On failure *out is NULL and
 * the error code and byte offset are written through. */
json_err json_parse(const void *buf, size_t len, json **out, size_t *errpos)
{
	P x;
	json *j;

	/* NULL with len 0 is a legitimate call and `NULL + 0` is undefined
	 * behaviour that UBSan reports, so substitute a real address. */
	x.b = x.p = buf ? (const unsigned char *)buf : (const unsigned char *)"";
	x.end = x.b + len;
	x.depth = 0;
	x.err = JSON_OK;
	x.errpos = 0;

	*out = NULL;
	skip_ws(&x);
	j = parse_value(&x);
	if (j) {
		skip_ws(&x);
		if (x.p != x.end)
			fail(&x, JSON_ERR_TRAILING, x.p);
	}
	if (x.err != JSON_OK) {
		json_free(j);
		if (errpos)
			*errpos = x.errpos;
		return x.err;
	}
	*out = j;
	return JSON_OK;
}

/* ---------------------------------------------------------------- lookup --- */

size_t json_len(const json *j)
{
	if (!j)
		return 0;
	if (j->type == JSON_ARRAY)
		return j->u.array.len;
	if (j->type == JSON_OBJECT)
		return j->u.object.len;
	return 0;
}

const json *json_at(const json *j, size_t i)
{
	if (!j || j->type != JSON_ARRAY || i >= j->u.array.len)
		return NULL;
	return j->u.array.v[i];
}

/* Last match wins, matching JavaScript and Python.
 * shortcut: linear scan, so an object with a thousand keys costs a thousand
 * memcmp per lookup; a hash index is the upgrade if anything ever calls this
 * in a loop.  The CLI never calls it, and the tests call it a few times. */
const json *json_get(const json *j, const char *key)
{
	size_t i, n;
	const json *hit = NULL;
	if (!j || j->type != JSON_OBJECT)
		return NULL;
	n = strlen(key);
	for (i = 0; i < j->u.object.len; i++)
		if (j->u.object.v[i].klen == n &&
		    memcmp(j->u.object.v[i].key, key, n) == 0)
			hit = j->u.object.v[i].val;
	return hit;
}

/* ------------------------------------------------------------ serialiser --- */

/* Canonical number form: of the 17 candidate spellings "%.1g" ... "%.17g",
 * keep the SHORTEST one that reads back as the same double.
 *
 * Neither half of that is optional.  "%.17g" always round-trips but prints 0.1
 * as "0.10000000000000001" and 100 as "100.00000000000000".  Taking the first
 * precision that round-trips instead of the shortest result prints 100 as
 * "1e+02", because %g switches to exponential notation when the decimal
 * exponent is >= the precision -- so the style depends on the precision and
 * not just on the value.  Comparing the strings fixes both: 100 -> "100",
 * 0.1 -> "0.1", 1e20 -> "1e+20", DBL_MIN_DENORM -> "5e-324".
 *
 * This is a definition, not a discovery, and the differential oracle in
 * t/diff.py implements the same four lines in Python so the two can compare
 * serialised text instead of parsed values.
 *
 * shortcut: 17 snprintf plus 17 strtod per number, which is maybe 2us.  A
 * Ryu or Grisu shortest-float printer is the upgrade if anyone ever
 * serialises enough numbers to notice; -c on the whole 354KB corpus takes
 * well under a second as it is. */
static void write_number(FILE *f, double d)
{
	char best[48], cand[48];
	int p;

	best[0] = 0;
	for (p = 1; p <= 17; p++) {
		snprintf(cand, sizeof cand, "%.*g", p, d);
		if (strtod(cand, NULL) != d)
			continue;
		if (!best[0] || strlen(cand) < strlen(best))
			memcpy(best, cand, strlen(cand) + 1);
	}
	/* best[0] is always set: %.17g round-trips by construction. */
	fputs(best, f);
}

/* RFC 8259 section 7 requires escaping only '"', '\' and %x00-1F.  Everything
 * else goes out as it came in, which means valid UTF-8 stays valid UTF-8 and
 * the output is byte-identical to the input for the common case.  '/' is NOT
 * escaped (legal either way, and escaping it is a HTML-in-JSON habit), DEL is
 * not escaped, and no \u escape is emitted for anything above U+001F, so
 * serialise(parse(s)) never produces a surrogate pair we would then have to
 * read back. */
static void write_string(FILE *f, const char *s, size_t n)
{
	size_t i;
	fputc('"', f);
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"':  fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\b': fputs("\\b", f); break;
		case '\f': fputs("\\f", f); break;
		case '\n': fputs("\\n", f); break;
		case '\r': fputs("\\r", f); break;
		case '\t': fputs("\\t", f); break;
		default:
			if (c < 0x20)
				fprintf(f, "\\u%04x", c);
			else
				fputc(c, f);
		}
	}
	fputc('"', f);
}

static void nl_indent(FILE *f, int indent, int level)
{
	int i;
	if (!indent)
		return;
	fputc('\n', f);
	for (i = 0; i < indent * level; i++)
		fputc(' ', f);
}

static void write_value(FILE *f, const json *j, int indent, int level)
{
	size_t i;

	switch (j->type) {
	case JSON_NULL:
		fputs("null", f);
		break;
	case JSON_BOOL:
		fputs(j->u.boolean ? "true" : "false", f);
		break;
	case JSON_NUMBER:
		write_number(f, j->u.number);
		break;
	case JSON_STRING:
		write_string(f, j->u.string.p, j->u.string.len);
		break;
	case JSON_ARRAY:
		/* An empty container stays on one line even when indenting,
		 * which is what json.dumps(indent=2) does, and the pretty
		 * printer is compared against it byte for byte. */
		if (!j->u.array.len) {
			fputs("[]", f);
			break;
		}
		fputc('[', f);
		for (i = 0; i < j->u.array.len; i++) {
			if (i)
				fputc(',', f);
			nl_indent(f, indent, level + 1);
			write_value(f, j->u.array.v[i], indent, level + 1);
		}
		nl_indent(f, indent, level);
		fputc(']', f);
		break;
	case JSON_OBJECT:
		if (!j->u.object.len) {
			fputs("{}", f);
			break;
		}
		fputc('{', f);
		for (i = 0; i < j->u.object.len; i++) {
			if (i)
				fputc(',', f);
			nl_indent(f, indent, level + 1);
			write_string(f, j->u.object.v[i].key,
				     j->u.object.v[i].klen);
			fputs(indent ? ": " : ":", f);
			write_value(f, j->u.object.v[i].val, indent, level + 1);
		}
		nl_indent(f, indent, level);
		fputc('}', f);
		break;
	}
}

/* indent == 0 is the compact canonical form: no space anywhere outside a
 * string.  indent > 0 pretty-prints with that many spaces per level. */
void json_write(FILE *f, const json *j, int indent)
{
	if (j)
		write_value(f, j, indent, 0);
}

/* ------------------------------------------------------------------- CLI --- */

#ifndef JSONC_NO_MAIN

static char *slurp(const char *path, size_t *len)
{
	FILE *f = path ? fopen(path, "rb") : stdin;
	size_t cap = 1 << 16, n = 0;
	char *buf;

	if (!f)
		return NULL;
	if (!(buf = malloc(cap))) {
		if (path)
			fclose(f);
		return NULL;
	}
	for (;;) {
		size_t got = fread(buf + n, 1, cap - n, f);
		n += got;
		if (n < cap)
			break;
		{
			char *nb = realloc(buf, cap * 2);
			if (!nb) {
				free(buf);
				if (path)
					fclose(f);
				return NULL;
			}
			buf = nb;
			cap *= 2;
		}
	}
	if (ferror(f)) {
		free(buf);
		if (path)
			fclose(f);
		return NULL;
	}
	if (path)
		fclose(f);
	/* Shrink to the exact size: with -fsanitize=address a read past the
	 * last byte of the document is then a real heap-buffer-overflow and not
	 * a read into the 64KB of slack that fread left behind. */
	{
		char *nb = realloc(buf, n ? n : 1);
		if (nb)
			buf = nb;
	}
	*len = n;
	return buf;
}

static void usage(void)
{
	fputs("usage: jsonc [-p|-c|-S] [FILE]\n"
	      "  (no flag)  validate only: exit 0 if valid, 1 if not\n"
	      "  -p         pretty-print (2-space indent)\n"
	      "  -c         compact canonical serialisation\n"
	      "  -S         truncation sweep: parse every prefix, report the count\n",
	      stderr);
}

/* Parse every prefix of the input and throw the results away.  Nothing about
 * the verdicts matters -- a prefix of a valid document is usually invalid --
 * what matters is that none of the thousands of truncated, malformed documents
 * reads off the end of the buffer, double-frees, or leaks.  Under
 * -fsanitize=address,undefined this is the only test in the suite that can
 * find an out-of-bounds read, and truncation is the input shape that produces
 * them: every "is there another byte?" check in the parser is exercised at the
 * exact offset where the answer is no.
 *
 * shortcut: for inputs over 8KB the sweep steps by len/4096 instead of 1, so
 * the two 100KB+ corpus files get 4096 cut points rather than 250001.  Every
 * byte offset of every file under 8KB -- 315 of the 318 -- is still swept.
 * Raising the threshold is a one-character change if it ever seems worth the
 * minutes. */
static void sweep(const char *buf, size_t len)
{
	size_t off, step = len > 8192 ? len / 4096 : 1, n = 0, acc = 0;

	for (off = 0; off <= len; off += step) {
		json *t;
		size_t ep;
		/* Each prefix gets its own exactly-sized allocation.  Handing
		 * json_parse a short length into a long buffer hides every
		 * past-the-end read from the sanitizer, because the bytes it
		 * reads are still inside the allocation -- which is how this
		 * sweep managed to pass with the length check in hex4() deleted.
		 * An exact malloc turns those reads back into heap-buffer-
		 * overflow reports. */
		char *cut = off ? malloc(off) : NULL;
		if (off && !cut)
			continue;
		if (off)
			memcpy(cut, buf, off);
		n++;
		if (json_parse(cut, off, &t, &ep) == JSON_OK) {
			acc++;
			json_free(t);
		}
		free(cut);
	}
	printf("swept %zu prefixes of %zu bytes (step %zu), %zu accepted\n",
	       n, len, step, acc);
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int mode = 0, i;
	char *buf;
	size_t len = 0, errpos = 0;
	json *j;
	json_err e;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] && !argv[i][2]) {
			switch (argv[i][1]) {
			case 'p': case 'c': case 'S':
				mode = argv[i][1];
				continue;
			default:
				usage();
				return 2;
			}
		}
		if (path) {
			usage();
			return 2;
		}
		path = argv[i];
	}

	if (!(buf = slurp(path, &len))) {
		fprintf(stderr, "jsonc: %s: cannot read\n", path ? path : "<stdin>");
		return 2;
	}
	if (mode == 'S') {
		sweep(buf, len);
		free(buf);
		return 0;
	}
	e = json_parse(buf, len, &j, &errpos);
	if (e != JSON_OK) {
		fprintf(stderr, "jsonc: %s:%zu: %s\n", path ? path : "<stdin>",
			errpos, json_strerror(e));
		free(buf);
		return 1;
	}
	if (mode == 'c' || mode == 'p') {
		json_write(stdout, j, mode == 'p' ? 2 : 0);
		fputc('\n', stdout);
	}
	json_free(j);
	free(buf);
	return 0;
}

#endif /* JSONC_NO_MAIN */

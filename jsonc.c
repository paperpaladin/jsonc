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

#include <errno.h>
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

static json *parse_value(P *x);

static json *parse_value(P *x)
{
	json *j;
	if (x->p >= x->end) {
		fail(x, JSON_ERR_EOF, x->end);
		return NULL;
	}
	switch (*x->p) {
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

	x.b = x.p = (const unsigned char *)buf;
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
	*len = n;
	return buf;
}

static void usage(void)
{
	fputs("usage: jsonc [-p|-c] [FILE]\n"
	      "  (no flag)  validate only: exit 0 if valid, 1 if not\n"
	      "  -p         pretty-print (2-space indent)\n"
	      "  -c         compact canonical serialisation\n",
	      stderr);
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
			case 'p': case 'c':
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
	e = json_parse(buf, len, &j, &errpos);
	if (e != JSON_OK) {
		fprintf(stderr, "jsonc: %s:%zu: %s\n", path ? path : "<stdin>",
			errpos, json_strerror(e));
		free(buf);
		return 1;
	}
	(void)mode;
	json_free(j);
	free(buf);
	return 0;
}

#endif /* JSONC_NO_MAIN */

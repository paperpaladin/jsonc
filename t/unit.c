/* t/unit.c -- the parts of the API the CLI cannot reach.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The CLI only ever reports "valid" or "not valid", so everything about the
 * DOM, the error codes, the byte offsets and the embedded-NUL handling is
 * invisible from a shell.  This is that, and nothing else: no framework, one
 * ok()/nok() pair, exit status is the result.
 *
 * _POSIX_C_SOURCE is for open_memstream, which is how json_write() gets
 * compared without a temporary file.
 */
#define _POSIX_C_SOURCE 200809L
#define JSONC_NO_MAIN
#include "../jsonc.c"

#include <math.h>

static int fails;

static void ok(const char *m) { printf("ok - %s\n", m); }
static void nok(const char *m) { printf("not ok - %s\n", m); fails++; }
static void chk(int cond, const char *m) { if (cond) ok(m); else nok(m); }

/* --- error codes and the offset they point at ---------------------------- */

static void test_errors(void)
{
	struct { const char *in; json_err code; size_t off; const char *why; } t[] = {
		{ "",            JSON_ERR_EOF,       0, "empty input" },
		{ "   ",         JSON_ERR_EOF,       3, "whitespace only" },
		{ "nul",         JSON_ERR_EOF,       3, "truncated null" },
		{ "nulx",        JSON_ERR_TOKEN,     0, "misspelled null" },
		{ "null x",      JSON_ERR_TRAILING,  5, "trailing garbage" },
		{ "01",          JSON_ERR_NUMBER,    1, "leading zero" },
		{ "1.",          JSON_ERR_EOF,       2, "number ends at the point" },
		{ "1.x",         JSON_ERR_NUMBER,    2, "non-digit after the point" },
		{ "1e",          JSON_ERR_EOF,       2, "number ends at the exponent" },
		{ "1e+x",        JSON_ERR_NUMBER,    3, "non-digit after exponent sign" },
		{ "1e999",       JSON_ERR_RANGE,     0, "overflow points at the token" },
		{ "\"a",         JSON_ERR_EOF,       2, "unterminated string" },
		{ "\"\\q\"",     JSON_ERR_STRING,    2, "bad escape points at the letter" },
		{ "\"\\u00g0\"", JSON_ERR_STRING,    5, "bad hex digit" },
		{ "\"\\ud800\"", JSON_ERR_SURROGATE, 1, "lone surrogate points at the backslash" },
		{ "\"\xc3\"",    JSON_ERR_UTF8,      2, "UTF-8 lead byte with a bad continuation" },
		{ "\"\xc3",      JSON_ERR_UTF8,      2, "UTF-8 sequence cut off by end of input" },
		{ "\"\xff\"",    JSON_ERR_UTF8,      1, "invalid lead byte" },
		{ "\"\x01\"",    JSON_ERR_STRING,    1, "bare control character" },
		{ "[1 2]",       JSON_ERR_COMMA,     3, "missing comma in an array" },
		{ "{\"a\" 1}",   JSON_ERR_COMMA,     5, "missing colon in an object" },
		{ "[1,",         JSON_ERR_EOF,       3, "array ends after a comma" },
		{ "{",           JSON_ERR_EOF,       1, "object ends after the brace" },
	};
	size_t i;
	char msg[160];

	for (i = 0; i < sizeof t / sizeof *t; i++) {
		json *j = NULL;
		size_t off = (size_t)-1;
		json_err e = json_parse(t[i].in, strlen(t[i].in), &j, &off);
		snprintf(msg, sizeof msg, "%s: %s at %zu", t[i].why,
			 json_strerror(t[i].code), t[i].off);
		if (e == t[i].code && off == t[i].off && j == NULL)
			ok(msg);
		else {
			char bad[200];
			snprintf(bad, sizeof bad, "%s (got %s at %zu, tree %s)",
				 msg, json_strerror(e), off, j ? "non-NULL" : "NULL");
			nok(bad);
		}
		json_free(j);
	}
	chk(strcmp(json_strerror((json_err)999), "ok") == 0,
	    "json_strerror clamps an out-of-range code instead of reading past the table");
}

/* --- the DOM ------------------------------------------------------------- */

static void test_dom(void)
{
	json *j = NULL;
	const json *v;
	size_t off;

	/* An escaped NUL is a byte in the middle of a string, not a terminator.
	 * strlen() on the decoded buffer would say 1; the length says 3. */
	chk(json_parse("\"a\\u0000b\"", 10, &j, &off) == JSON_OK, "parse a string with an escaped NUL");
	chk(j && j->type == JSON_STRING && j->u.string.len == 3 &&
	    memcmp(j->u.string.p, "a\0b", 3) == 0,
	    "an escaped NUL is a byte in the string, length 3");
	chk(j && j->u.string.p[3] == 0, "decoded strings are also NUL-terminated");
	json_free(j);

	/* Duplicate keys: all kept in order, json_get returns the last. */
	chk(json_parse("{\"a\":1,\"b\":2,\"a\":3}", 19, &j, &off) == JSON_OK,
	    "parse an object with a duplicate key");
	chk(json_len(j) == 3, "all three members are kept");
	v = json_get(j, "a");
	chk(v && v->type == JSON_NUMBER && v->u.number == 3.0,
	    "json_get returns the LAST duplicate");
	chk(json_get(j, "b") != NULL, "json_get finds a non-duplicated key");
	chk(json_get(j, "c") == NULL, "json_get returns NULL for a missing key");
	chk(json_get(j, "") == NULL, "json_get returns NULL for the empty key when absent");
	chk(json_at(j, 0) == NULL, "json_at on an object returns NULL");
	json_free(j);

	chk(json_parse("[10,20]", 7, &j, &off) == JSON_OK, "parse an array");
	chk(json_len(j) == 2, "json_len counts array elements");
	chk(json_at(j, 1) && json_at(j, 1)->u.number == 20.0, "json_at indexes");
	chk(json_at(j, 2) == NULL, "json_at past the end returns NULL");
	chk(json_get(j, "a") == NULL, "json_get on an array returns NULL");
	json_free(j);

	chk(json_parse("7", 1, &j, &off) == JSON_OK, "parse a bare number");
	chk(json_len(j) == 0, "json_len on a scalar is 0");
	chk(json_at(j, 0) == NULL, "json_at on a scalar is NULL");
	json_free(j);

	/* Signed zero survives, which is the one place we differ from CPython. */
	chk(json_parse("-0", 2, &j, &off) == JSON_OK, "parse -0");
	chk(j && j->u.number == 0.0 && signbit(j->u.number),
	    "-0 keeps its sign bit");
	json_free(j);

	chk(json_parse("{\"\":[]}", 7, &j, &off) == JSON_OK, "parse an empty key");
	chk(json_len(j) == 1 && j->u.object.v[0].klen == 0,
	    "the empty key has length 0");
	chk(json_get(j, "") != NULL, "json_get finds the empty key");
	json_free(j);

	/* NULL/zero-length input must be an ordinary error, not a crash. */
	chk(json_parse(NULL, 0, &j, &off) == JSON_ERR_EOF && j == NULL,
	    "json_parse(NULL, 0) is JSON_ERR_EOF, not a dereference");
	chk(json_parse("[]", 2, &j, NULL) == JSON_OK,
	    "errpos may be NULL on success");
	json_free(j);
	chk(json_parse("[", 1, &j, NULL) == JSON_ERR_EOF,
	    "errpos may be NULL on failure");

	/* A length shorter than the buffer must be respected, not strlen'd. */
	chk(json_parse("[1,2]garbage", 5, &j, &off) == JSON_OK,
	    "json_parse honours len and ignores the bytes after it");
	json_free(j);

	json_free(NULL);
	ok("json_free(NULL) is a no-op");
}

/* --- the depth limit, as a return value --------------------------------- */

static void test_depth(void)
{
	size_t n, off;
	char *s;
	json *j;

	for (n = JSONC_MAX_DEPTH; n <= JSONC_MAX_DEPTH + 1; n++) {
		json_err e;
		char msg[80];
		s = malloc(2 * n + 1);
		if (!s) { nok("malloc for the depth test"); return; }
		memset(s, '[', n);
		memset(s + n, ']', n);
		e = json_parse(s, 2 * n, &j, &off);
		snprintf(msg, sizeof msg, "%zu nested arrays: %s",
			 n, n <= JSONC_MAX_DEPTH ? "accepted" : "JSON_ERR_DEPTH");
		if (n <= JSONC_MAX_DEPTH)
			chk(e == JSON_OK && j != NULL, msg);
		else
			chk(e == JSON_ERR_DEPTH && j == NULL && off == JSONC_MAX_DEPTH, msg);
		json_free(j);
		free(s);
	}
}

/* --- serialise into memory, reparse, compare ---------------------------- */

static char *dump(const json *j, int indent)
{
	char *p = NULL;
	size_t n = 0;
	FILE *f = open_memstream(&p, &n);
	if (!f)
		return NULL;
	json_write(f, j, indent);
	fclose(f);
	return p;
}

static void test_write(void)
{
	static const char *docs[] = {
		"null", "0", "-0", "1e999999999999",	/* the last one is an error */
		"[]", "{}", "[[],{},[[{}]]]",
		"{\"a\":1,\"a\":2}",
		"\"\\u0000\\u001f\\\"\\\\\\u007f\"",
		"\"\\ud83d\\ude00\"",
		"[0.1,1e20,5e-324,1.7976931348623157e308,9007199254740993]",
		"{\"k\":[true,false,null,{\"\":[]}]}",
	};
	size_t i;

	for (i = 0; i < sizeof docs / sizeof *docs; i++) {
		json *a = NULL, *b = NULL;
		size_t off;
		char *x, *y, msg[200];

		if (json_parse(docs[i], strlen(docs[i]), &a, &off) != JSON_OK)
			continue;	/* rejected inputs are tested elsewhere */
		if (!(x = dump(a, 0))) { nok("open_memstream"); json_free(a); return; }
		if (json_parse(x, strlen(x), &b, &off) != JSON_OK) {
			snprintf(msg, sizeof msg, "our own output re-parses: %s -> %s",
				 docs[i], x);
			nok(msg);
			free(x);
			json_free(a);
			continue;
		}
		y = dump(b, 0);
		snprintf(msg, sizeof msg, "write/parse/write is a fixed point: %s -> %s",
			 docs[i], x);
		chk(y && strcmp(x, y) == 0, msg);
		free(x);
		free(y);
		json_free(a);
		json_free(b);
	}

	/* Indent only affects whitespace outside strings: the compact form of a
	 * pretty-printed document is the compact form of the original. */
	{
		const char *doc = "{\"a\":[1,{\"b\":\"x y\"},[]],\"c\":{}}";
		json *a = NULL, *b = NULL;
		size_t off;
		char *pretty, *c1, *c2;
		json_parse(doc, strlen(doc), &a, &off);
		pretty = dump(a, 2);
		json_parse(pretty, strlen(pretty), &b, &off);
		c1 = dump(a, 0);
		c2 = dump(b, 0);
		chk(c1 && c2 && strcmp(c1, c2) == 0,
		    "indent changes only whitespace outside strings");
		chk(strchr(pretty, '\n') != NULL, "indent 2 actually emits newlines");
		free(pretty); free(c1); free(c2);
		json_free(a); json_free(b);
	}

	{
		char *empty = dump(NULL, 0);
		chk(empty && empty[0] == 0,
		    "json_write(NULL) writes nothing rather than crashing");
		free(empty);
	}
}

int main(void)
{
	test_errors();
	test_dom();
	test_depth();
	test_write();
	return fails ? 1 : 0;
}

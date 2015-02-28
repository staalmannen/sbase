/* FIXME: summary
 * decide whether we enforce valid UTF-8, right now it's enforced in certain
 *     parts of the script, but not the input...
 * nul bytes cause explosions due to use of libc string functions. thoughts?
 * lack of newline at end of file, currently we add one. what should we do?
 * allow "\\t" for "\t" etc. in regex? in replacement text?
 * POSIX says don't flush on N when out of input, but GNU and busybox do.
 */

#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "utf.h"
#include "util.h"

/* Types */

/* used as queue for writes and stack for {,:,b,t */
typedef struct {
	void **data;
	size_t size;
	size_t cap;
} Vec;

/* used for arbitrary growth, str is a C string
 * FIXME: does it make sense to keep track of length? or just rely on libc
 *        string functions? If we want to support nul bytes everything changes
 */
typedef struct {
	char  *str;
	size_t cap;
} String;

typedef struct Cmd Cmd;
typedef struct {
	void  (*fn)(Cmd *);
	char *(*getarg)(Cmd *, char *);
	void  (*freearg)(Cmd *);
	unsigned char naddr;
} Fninfo;

typedef struct {
	union {
		size_t   lineno;
		regex_t *re;
	} u;
	enum {
		IGNORE, /* empty address, ignore        */
		EVERY , /* every line                   */
		LINE  , /* ilne number                  */
		LAST  , /* last line ($)                */
		REGEX , /* use included regex           */
		LASTRE, /* use most recently used regex */
	} type;
} Addr;

/* DISCUSS: naddr is not strictly necessary, but very helpful
 * naddr == 0 iff beg.type == EVERY  && end.type == IGNORE
 * naddr == 1 iff beg.type != IGNORE && end.type == IGNORE
 * naddr == 2 iff beg.type != IGNORE && end.type != IGNORE
 */
typedef struct {
	Addr          beg;
	Addr          end;
	unsigned char naddr;
} Range;

typedef struct {
	regex_t      *re; /* if NULL use last regex */
	String        repl;
	FILE         *file;
	size_t        occurrence; /* 0 for all (g flag) */
	Rune          delim;
	unsigned int  p:1;
} Sarg;

typedef struct {
	Rune *set1;
	Rune *set2;
} Yarg;

typedef struct {
	String str; /* a,c,i text. r file path */
	void  (*print)(char *, FILE *); /* check_puts for a, write_file for r, unused for c,i */
} ACIRarg;

struct Cmd {
	Range   range;
	Fninfo *fninfo;
	union {
		Cmd      *jump;   /* used for   b,t when running  */
		char     *label;  /* used for :,b,t when building */
		ptrdiff_t offset; /* used for { (pointers break during realloc) */
		FILE     *file;   /* used for w */

		/* FIXME: Should the following be in the union? or pointers and malloc? */
		Sarg      s;
		Yarg      y;
		ACIRarg   acir;
	} u; /* I find your lack of anonymous unions disturbing */
	unsigned int in_match:1;
	unsigned int negate  :1;
};

/* Files for w command (and s' w flag) */
typedef struct {
	char *path;
	FILE *file;
} Wfile;

/*
 * Function Declarations
 */

/* Dynamically allocated arrays and strings */
void resize(void **ptr, size_t *nmemb, size_t size, size_t new_nmemb, void **next);
void *pop(Vec *v);
void push(Vec *v, void *p);
void stracat(String *dst, char *src);
void strnacat(String *dst, char *src, size_t n);
void stracpy(String *dst, char *src);
void strnacpy(String *dst, char *src, size_t n);

/* Cleanup and errors */
static void usage(void);

/* Parsing functions and related utilities */
void compile(char *s, int isfile);
int read_line(FILE *f, String *s);
char *make_range(Range *range, char *s);
char *make_addr(Addr *addr, char *s);
char *find_delim(char *s, Rune delim, int do_brackets);
char *chompr(char *s, Rune rune);
char *chomp(char *s);
Rune *strtorunes(char *s, size_t nrunes);
long stol(char *s, char **endp);
size_t escapes(char *beg, char *end, Rune delim, int n_newline);
size_t echarntorune(Rune *r, char *s, size_t n);
void insert_labels(void);

/* Get and Free arg and related utilities */
char *get_aci_arg(Cmd *c, char *s);
void aci_append(Cmd *c, char *s);
void free_acir_arg(Cmd *c);
char *get_bt_arg(Cmd *c, char *s);
char *get_r_arg(Cmd *c, char *s);
char *get_s_arg(Cmd *c, char *s);
void free_s_arg(Cmd *c);
char *get_w_arg(Cmd *c, char *s);
char *get_y_arg(Cmd *c, char *s);
void free_y_arg(Cmd *c);
char *get_colon_arg(Cmd *c, char *s);
char *get_lbrace_arg(Cmd *c, char *s);
char *get_rbrace_arg(Cmd *c, char *s);
char *semicolon_arg(char *s);

/* Running */
void run(void);
int in_range(Cmd *c);
int match_addr(Addr *a);
int next_file(void);
int is_eof(FILE *f);
void do_writes(void);
void write_file(char *path, FILE *out);
void check_puts(char *s, FILE *f);
void update_ranges(Cmd *beg, Cmd *end);

/* Sed functions */
void cmd_y(Cmd *c);
void cmd_x(Cmd *c);
void cmd_w(Cmd *c);
void cmd_t(Cmd *c);
void cmd_s(Cmd *c);
void cmd_r(Cmd *c);
void cmd_q(Cmd *c);
void cmd_P(Cmd *c);
void cmd_p(Cmd *c);
void cmd_N(Cmd *c);
void cmd_n(Cmd *c);
void cmd_l(Cmd *c);
void cmd_i(Cmd *c);
void cmd_H(Cmd *c);
void cmd_h(Cmd *c);
void cmd_G(Cmd *c);
void cmd_g(Cmd *c);
void cmd_D(Cmd *c);
void cmd_d(Cmd *c);
void cmd_c(Cmd *c);
void cmd_b(Cmd *c);
void cmd_a(Cmd *c);
void cmd_colon(Cmd *c);
void cmd_equal(Cmd *c);
void cmd_lbrace(Cmd *c);
void cmd_rbrace(Cmd *c);
void cmd_last(Cmd *c);

/* Actions */
void new_line(void);
void app_line(void);
void new_next(void);
void old_next(void);

/*
 * Globals
 */
Vec braces, labels, branches; /* holds ptrdiff_t. addrs of {, :, bt */
Vec writes; /* holds cmd*. writes scheduled by a and r commands */
Vec wfiles; /* holds Wfile*. files for w and s///w commands */

Cmd   *prog, *pc; /* Program, program counter */
size_t pcap;
size_t lineno;

regex_t *lastre; /* last used regex for empty regex search */
char   **files;  /* list of file names from argv */
FILE    *file;   /* current file we are reading */

String patt, hold, genbuf;

struct {
	unsigned int n       :1; /* -n (no print) */
	unsigned int s       :1; /* s/// replacement happened */
	unsigned int aci_cont:1; /* a,c,i text continuation */
	unsigned int s_cont  :1; /* s/// replacement text continuation */
	unsigned int halt    :1; /* halt execution */
} gflags;

/* FIXME: move character inside Fninfo and only use 26*sizeof(Fninfo) instead of 127*sizeof(Fninfo) bytes */
Fninfo fns[] = {
	['a'] = { cmd_a     , get_aci_arg   , free_acir_arg , 1 }, /* schedule write of text for later                                                      */
	['b'] = { cmd_b     , get_bt_arg    , NULL          , 2 }, /* branch to label char *label when building, Cmd *jump when running                     */
	['c'] = { cmd_c     , get_aci_arg   , free_acir_arg , 2 }, /* delete pattern space. at 0 or 1 addr or end of 2 addr, write text                     */
	['d'] = { cmd_d     , NULL          , NULL          , 2 }, /* delete pattern space                                                                  */
	['D'] = { cmd_D     , NULL          , NULL          , 2 }, /* delete to first newline and start new cycle without reading (if no newline, d)        */
	['g'] = { cmd_g     , NULL          , NULL          , 2 }, /* replace pattern space with hold space                                                 */
	['G'] = { cmd_G     , NULL          , NULL          , 2 }, /* append newline and hold space to pattern space                                        */
	['h'] = { cmd_h     , NULL          , NULL          , 2 }, /* replace hold space with pattern space                                                 */
	['H'] = { cmd_H     , NULL          , NULL          , 2 }, /* append newline and pattern space to hold space                                        */
	['i'] = { cmd_i     , get_aci_arg   , free_acir_arg , 1 }, /* write text                                                                            */
	['l'] = { cmd_l     , NULL          , NULL          , 2 }, /* write pattern space in 'visually unambiguous form'                                    */
	['n'] = { cmd_n     , NULL          , NULL          , 2 }, /* write pattern space (unless -n) read to replace pattern space (if no input, quit)     */
	['N'] = { cmd_N     , NULL          , NULL          , 2 }, /* append to pattern space separated by newline, line number changes (if no input, quit) */
	['p'] = { cmd_p     , NULL          , NULL          , 2 }, /* write pattern space                                                                   */
	['P'] = { cmd_P     , NULL          , NULL          , 2 }, /* write pattern space up to first newline                                               */
	['q'] = { cmd_q     , NULL          , NULL          , 1 }, /* quit                                                                                  */
	['r'] = { cmd_r     , get_r_arg     , free_acir_arg , 1 }, /* write contents of file (unable to open/read treated as empty file)                    */
	['s'] = { cmd_s     , get_s_arg     , free_s_arg    , 2 }, /* find/replace/all that crazy s stuff                                                   */
	['t'] = { cmd_t     , get_bt_arg    , NULL          , 2 }, /* if s/// succeeded (since input or last t) branch to label (branch to end if no label) */
	['w'] = { cmd_w     , get_w_arg     , NULL          , 2 }, /* append pattern space to file                                                          */
	['x'] = { cmd_x     , NULL          , NULL          , 2 }, /* exchange pattern and hold spaces                                                      */
	['y'] = { cmd_y     , get_y_arg     , free_y_arg    , 2 }, /* replace runes in set1 with runes in set2                                              */
	[':'] = { cmd_colon , get_colon_arg , NULL          , 0 }, /* defines label for later b and t commands                                              */
	['='] = { cmd_equal , NULL          , NULL          , 1 }, /* printf("%d\n", line_number);                                                          */
	['{'] = { cmd_lbrace, get_lbrace_arg, NULL          , 2 }, /* if we match, run commands, otherwise jump to close                                    */
	['}'] = { cmd_rbrace, get_rbrace_arg, NULL          , 0 }, /* noop, hold onto open for ease of building scripts                                     */

	[0x7f] = { NULL, NULL, NULL, 0 }, /* index is checked with isascii(3p). fill out rest of array */
};


/*
 * Function Definitions
 */

/* given memory pointed to by *ptr that currently holds *nmemb members of size
 * size, realloc to hold new_nmemb members, return new_nmemb in *memb and one
 * past old end in *next. if realloc fails...explode
 */
void
resize(void **ptr, size_t *nmemb, size_t size, size_t new_nmemb, void **next)
{
	void *n, *tmp;

	if (new_nmemb) {
		tmp = erealloc(*ptr, new_nmemb * size);
	} else { /* turns out realloc(*ptr, 0) != free(*ptr) */
		free(*ptr);
		tmp = NULL;
	}
	n = (char *)tmp + *nmemb * size;
	*nmemb = new_nmemb;
	*ptr   = tmp;
	if (next)
		*next = n;
}

void *
pop(Vec *v)
{
	if (!v->size)
		return NULL;
	return v->data[--v->size];
}

void
push(Vec *v, void *p)
{
	if (v->size == v->cap)
		resize((void **)&v->data, &v->cap, sizeof(*v->data), v->cap * 2 + 1, NULL);
	v->data[v->size++] = p;
}

void
stracat(String *dst, char *src)
{
	int new = !dst->cap;
	size_t len;

	len = (new ? 0 : strlen(dst->str)) + strlen(src) + 1;
	if (dst->cap < len)
		resize((void **)&dst->str, &dst->cap, 1, len * 2, NULL);
	if (new)
		*dst->str = '\0';
	strcat(dst->str, src);
}

void
strnacat(String *dst, char *src, size_t n)
{
	int new = !dst->cap;
	size_t len;

	len = strlen(src);
	len = (new ? 0 : strlen(dst->str)) + MIN(n, len) + 1;
	if (dst->cap < len)
		resize((void **)&dst->str, &dst->cap, 1, len * 2, NULL);
	if (new)
		*dst->str = '\0';
	strlcat(dst->str, src, len);
}

void
stracpy(String *dst, char *src)
{
	size_t len;

	len = strlen(src) + 1;
	if (dst->cap < len)
		resize((void **)&dst->str, &dst->cap, 1, len * 2, NULL);
	strcpy(dst->str, src);
}

void
strnacpy(String *dst, char *src, size_t n)
{
	size_t len;

	len = strlen(src);
	len = strlen(dst->str) + MIN(n, len) + 1;
	if (dst->cap < len)
		resize((void **)&dst->str, &dst->cap, 1, len * 2, NULL);
	strlcpy(dst->str, src, len);
}

void
leprintf(char *s)
{
	if (errno)
		eprintf("%zu: %s: %s\n", lineno, s, strerror(errno));
	else
		eprintf("%zu: %s\n", lineno, s);
}

/* FIXME: write usage message */
static void
usage(void)
{
	eprintf("USAGE\n");
}

/* Differences from POSIX
 * we allows semicolons and trailing blanks inside {}
 * we allow spaces after ! (and in between !s)
 */
void
compile(char *s, int isfile)
{
	FILE *f;

	if (!isfile && !*s) /* empty string script */
		return;

	f = isfile ? fopen(s, "r") : fmemopen(s, strlen(s), "r");
	if (!f)
		eprintf("fopen/fmemopen failed\n");

	/* NOTE: get arg functions can't use genbuf */
	while (read_line(f, &genbuf) != EOF) {
		s = genbuf.str;

		/* if the first two characters of the script are "#n" default output shall be suppressed */
		if (++lineno == 1 && *s == '#' && s[1] == 'n') {
			gflags.n = 1;
			continue;
		}

		if (gflags.aci_cont) {
			aci_append(pc - 1, s);
			continue;
		}
		if (gflags.s_cont)
			s = (pc - 1)->fninfo->getarg(pc - 1, s);

		while (*s) {
			s = chompr(s, ';');
			if (!*s || *s == '#')
				break;

			if ((size_t)(pc - prog) == pcap)
				resize((void **)&prog, &pcap, sizeof(*prog), pcap * 2 + 1, (void **)&pc);

			pc->range.beg.type = pc->range.end.type = IGNORE;
			pc->fninfo = NULL;
			pc->in_match = 0;

			s = make_range(&pc->range, s);
			s = chomp(s);
			pc->negate = *s == '!';
			s = chompr(s, '!');

			if (!isascii(*s) || !(pc->fninfo = &fns[(unsigned)*s])->fn)
				leprintf("bad sed function");
			if (pc->range.naddr > pc->fninfo->naddr)
				leprintf("wrong number of addresses");
			s++;

			if (pc->fninfo->getarg)
				s = pc->fninfo->getarg(pc, s);

			pc++;
		}
	}

	if (fclose(f))
		weprintf("fclose failed\n");
}

/* FIXME: if we decide to honor lack of trailing newline, set/clear a global
 * flag when reading a line
 */
int
read_line(FILE *f, String *s)
{
	ssize_t len;

	if (!f)
		return EOF;

	if ((len = getline(&s->str, &s->cap, f)) < 0) {
		if (ferror(f))
			eprintf("getline failed\n");
		return EOF;
	}
	if (s->str[--len] == '\n')
		s->str[len] = '\0';
	return 0;
}

/* read first range from s, return pointer to one past end of range */
char *
make_range(Range *range, char *s)
{
	s = make_addr(&range->beg, s);

	if (*s == ',')
		s = make_addr(&range->end, s + 1);
	else
		range->end.type = IGNORE;

	if      (range->beg.type == EVERY  && range->end.type == IGNORE) range->naddr = 0;
	else if (range->beg.type != IGNORE && range->end.type == IGNORE) range->naddr = 1;
	else if (range->beg.type != IGNORE && range->end.type != IGNORE) range->naddr = 2;
	else leprintf("this is impossible...");

	return s;
}

/* read first addr from s, return pointer to one past end of addr */
char *
make_addr(Addr *addr, char *s)
{
	Rune r;
	char *p = s + strlen(s);
	size_t rlen = echarntorune(&r, s, p - s);

	if (r == '$') {
		addr->type = LAST;
		s += rlen;
	} else if (isdigitrune(r)) {
		addr->type = LINE;
		addr->u.lineno = stol(s, &s);
	} else if (r == '/' || r == '\\') {
		Rune delim;
		if (r == '\\') {
			s += rlen;
			rlen = echarntorune(&r, s, p - s);
		}
		if (r == '\\')
			leprintf("bad delimiter '\\'");
		delim = r;
		s += rlen;
		rlen = echarntorune(&r, s, p - s);
		if (r == delim) {
			addr->type = LASTRE;
			s += rlen;
		} else {
			addr->type = REGEX;
			p = find_delim(s, delim, 1);
			if (!*p)
				leprintf("unclosed regex");
			p -= escapes(s, p, delim, 0);
			*p++ = '\0';
			addr->u.re = emalloc(sizeof(*addr->u.re));
			eregcomp(addr->u.re, s, 0);
			s = p;
		}
	} else {
		addr->type = EVERY;
	}

	return s;
}

/* return pointer to first delim in s that is not escaped
 * and if do_brackets is set, not in [] (note possible [::], [..], [==], inside [])
 * return pointer to trailing nul byte if no delim found
 *
 * any escaped character that is not special is just itself (POSIX undefined)
 * FIXME: pull out into some util thing, will be useful for ed as well
 */
char *
find_delim(char *s, Rune delim, int do_brackets)
{
	enum {
		OUTSIDE         , /* not in brackets */
		BRACKETS_OPENING, /* last char was first [ or last two were first [^ */
		BRACKETS_INSIDE , /* inside [] */
		INSIDE_OPENING  , /* inside [] and last char was [ */
		CLASS_INSIDE    , /* inside class [::], or colating element [..] or [==], inside [] */
		CLASS_CLOSING   , /* inside class [::], or colating element [..] or [==], and last character was the respective : . or = */
	} state = OUTSIDE;

	Rune r, c = 0; /* no c won't be used uninitialized, shutup -Wall */
	size_t rlen;
	int escape = 0;
	char *end = s + strlen(s);

	for (; *s; s += rlen) {
		rlen = echarntorune(&r, s, end - s);

		if      (state == BRACKETS_OPENING       &&  r == '^'  ) {                            continue; }
		else if (state == BRACKETS_OPENING       &&  r == ']'  ) { state  = BRACKETS_INSIDE ; continue; }
		else if (state == BRACKETS_OPENING                     ) { state  = BRACKETS_INSIDE ;           }

		if      (state == CLASS_CLOSING          &&  r == ']'  ) { state  = BRACKETS_INSIDE ;           }
		else if (state == CLASS_CLOSING                        ) { state  = CLASS_INSIDE    ;           }
		else if (state == CLASS_INSIDE           &&  r ==  c   ) { state  = CLASS_CLOSING   ;           }
		else if (state == INSIDE_OPENING         && (r == ':'  ||
		                                             r == '.'  ||
		                                             r == '=') ) { state  = CLASS_INSIDE    ; c = r;    }
		else if (state == INSIDE_OPENING         &&  r == ']'  ) { state  = OUTSIDE         ;           }
		else if (state == BRACKETS_INSIDE        &&  r == '['  ) { state  = INSIDE_OPENING  ;           }
		else if (state == BRACKETS_INSIDE        &&  r == ']'  ) { state  = OUTSIDE         ;           }
		else if (state == OUTSIDE                &&  escape    ) { escape = 0               ;           }
		else if (state == OUTSIDE                &&  r == '\\' ) { escape = 1               ;           }
		else if (state == OUTSIDE && do_brackets &&  r == '['  ) { state  = BRACKETS_OPENING;           }
		else if (state == OUTSIDE                &&  r == delim) return s;
	}
	return s;
}

char *
chomp(char *s)
{
	return chompr(s, 0);
}

/* eat all leading whitespace and occurrences of rune */
char *
chompr(char *s, Rune rune)
{
	Rune   r;
	size_t rlen;
	char  *end = s + strlen(s);

	while (*s && (rlen = echarntorune(&r, s, end - s)) && (isspacerune(r) || r == rune))
		s += rlen;
	return s;
}

/* convert first nrunes Runes from UTF-8 string s in allocated Rune*
 * NOTE: sequence must be valid UTF-8, check first */
Rune *
strtorunes(char *s, size_t nrunes)
{
	Rune *rs = emalloc(sizeof(*rs) * nrunes + 1), *rp = rs;

	while (nrunes--)
		s += chartorune(rp++, s);

	*rp = '\0';
	return rs;
}

long
stol(char *s, char **endp)
{
	long n;
	errno = 0;
	n = strtol(s, endp, 10);

	if (errno)
		leprintf("strtol:");
	if (*endp == s)
		leprintf("strtol: invalid number");

	return n;
}

/* from beg to end replace "\\d" with "d" and "\\n" with "\n" (where d is delim)
 * if delim is 'n' and n_newline is 0 then "\\n" is replaced with "n" (normal)
 * if delim is 'n' and n_newline is 1 then "\\n" is replaced with "\n" (y command)
 * if delim is 0 all escaped characters represent themselves (aci text)
 * memmove rest of string (beyond end) into place
 * return the number of converted escapes (backslashes removed)
 * FIXME: this has had too many corner cases slapped on and is ugly. rewrite better
 */
size_t
escapes(char *beg, char *end, Rune delim, int n_newline)
{
	size_t num = 0;
	char *src = beg, *dst = beg;

	while (src < end) {
		/* handle escaped backslash specially so we don't think the second
		 * backslash is escaping something */
		if (*src == '\\' && src[1] == '\\') {
			*dst++ = *src++;
			if (delim)
				*dst++ = *src++;
			else
				src++;
		} else if (*src == '\\' && !delim) {
			src++;
		} else if (*src == '\\' && src[1]) {
			Rune r;
			size_t rlen;
			num++;
			src++;
			rlen = echarntorune(&r, src, end - src);

			if (r == 'n' && delim == 'n') {
				*src = n_newline ? '\n' : 'n'; /* src so we can still memmove() */
			} else if (r == 'n') {
				*src = '\n';
			} else if (r != delim) {
				*dst++ = '\\';
				num--;
			}

			memmove(dst, src, rlen);
			dst += rlen;
			src += rlen;
		} else {
			*dst++ = *src++;
		}
	}
	memmove(dst, src, strlen(src) + 1);
	return num;
}

size_t
echarntorune(Rune *r, char *s, size_t n)
{
	size_t rlen = charntorune(r, s, n);
	if (!rlen || *r == Runeerror)
		leprintf("invalid UTF-8");
	return rlen;
}

void
insert_labels(void)
{
	while (branches.size) {
		Cmd *from = prog + (ptrdiff_t)pop(&branches);

		if (!from->u.label) {/* no label branch to end of script */
			from->u.jump = pc - 1;
		} else {
			size_t i;
			Cmd *to;

			for (i = 0; i < labels.size; i++) {
				to = prog + (ptrdiff_t)labels.data[i];
				if (!strcmp(from->u.label, to->u.label)) {
					from->u.jump = to;
					break;
				}
			}
			if (i == labels.size)
				leprintf("bad label");
		}
	}
}

/*
 * Getargs / Freeargs
 * Read argument from s, return pointer to one past last character of argument
 */

/* POSIX compliant
 * i\
 * foobar
 *
 * also allow the following non POSIX compliant
 * i        # empty line
 * ifoobar
 * ifoobar\
 * baz
 *
 * FIXME: GNU and busybox discard leading spaces
 * i  foobar
 * i foobar
 * ifoobar
 * are equivalent in GNU and busybox. We don't. Should we?
 */
char *
get_aci_arg(Cmd *c, char *s)
{
	c->u.acir.print = check_puts;
	c->u.acir.str = (String){ NULL, 0 };

	gflags.aci_cont = !!*s; /* no continue flag if empty string */

	/* neither empty string nor POSIX compliant */
	if (*s && !(*s == '\\' && !s[1]))
		aci_append(c, s);

	return s + strlen(s);
}

void
aci_append(Cmd *c, char *s)
{
	char *end = s + strlen(s), *p = end;

	gflags.aci_cont = 0;
	while (--p >= s && *p == '\\')
		gflags.aci_cont = !gflags.aci_cont;

	if (gflags.aci_cont)
		*--end = '\n';

	escapes(s, end, 0, 0);
	stracat(&c->u.acir.str, s);
}

void
free_acir_arg(Cmd *c)
{
	free(c->u.acir.str.str);
}

/* POSIX dictates that label is rest of line, including semicolons, trailing
 * whitespace, closing braces, etc. and can be limited to 8 bytes
 *
 * I allow a semicolon or closing brace to terminate a label name, it's not
 * POSIX compliant, but it's useful and every sed version I've tried to date
 * does the same.
 *
 * FIXME: POSIX dictates that leading whitespace is ignored but trailing
 * whitespace is not. This is annoying and we should probably get rid of it.
 */
char *
get_bt_arg(Cmd *c, char *s)
{
	char *p = semicolon_arg(s = chomp(s));

	if (p != s) {
		c->u.label = estrndup(s, p - s);
	} else {
		c->u.label = NULL;
	}

	push(&branches, (void *)(c - prog));
	return p;
}

/* POSIX dictates file name is rest of line including semicolons, trailing
 * whitespace, closing braces, etc. and file name must be preceded by a space
 *
 * I allow a semicolon or closing brace to terminate a file name and don't
 * enforce leading space.
 *
 * FIXME: decide whether trailing whitespace should be included and fix
 * accordingly
 */
char *
get_r_arg(Cmd *c, char *s)
{
	char *p = semicolon_arg(s = chomp(s));

	if (p == s)
		leprintf("no file name");

	c->u.acir.str.str = estrndup(s, p - s);
	c->u.acir.print = write_file;

	return p;
}

/* we allow "\\n" in replacement text to mean "\n" (undefined in POSIX)
 *
 * FIXME: allow other escapes in regex and replacement? if so change escapes()
 */
char *
get_s_arg(Cmd *c, char *s)
{
	Rune delim, r;
	Cmd buf;
	char *p;
	int esc;

	/* s/Find/Replace/Flags */

	/* Find */
	if (!gflags.s_cont) { /* NOT continuing from literal newline in replacement text */
		int lastre = 0;
		c->u.s.repl = (String){ NULL, 0 };
		c->u.s.occurrence = 1;
		c->u.s.file = NULL;
		c->u.s.p = 0;

		if (!*s || *s == '\\')
			leprintf("bad delimiter");

		p = s + strlen(s);
		s += echarntorune(&delim, s, p - s);
		c->u.s.delim = delim;

		echarntorune(&r, s, p - s);
		if (r == delim) /* empty regex */
			lastre = 1;

		p = find_delim(s, delim, 1);
		if (!*p)
			leprintf("missing second delimiter");
		p -= escapes(s, p, delim, 0);
		*p = '\0';

		if (lastre) {
			c->u.s.re = NULL;
		} else {
			c->u.s.re = emalloc(sizeof(*c->u.s.re));
			/* FIXME: different eregcomp that calls fatal */
			eregcomp(c->u.s.re, s, 0);
		}
		s = p + 1;
	}

	/* Replace */
	delim = c->u.s.delim;

	p = find_delim(s, delim, 0);
	p -= escapes(s, p, delim, 0);
	if (!*p) { /* no third delimiter */
		/* FIXME: same backslash counting as aci_append() */
		if (p[-1] != '\\')
			leprintf("missing third delimiter or <backslash><newline>");
		p[-1] = '\n';
		gflags.s_cont = 1;
	} else {
		gflags.s_cont = 0;
	}

	/* check for bad references in replacement text */
	*p = '\0';
	for (esc = 0, p = s; *p; p++) {
		if (esc) {
			esc = 0;
			if (isdigit(*p) && c->u.s.re && (size_t)(*p - '0') > c->u.s.re->re_nsub)
				leprintf("back reference number greater than number of groups");
		} else if (*p == '\\') {
			esc = 1;
		}
	}
	stracat(&c->u.s.repl, s);

	if (gflags.s_cont)
		return p;

	s = p + 1;

	/* Flags */
	p = semicolon_arg(s = chomp(s));

	/* FIXME: currently for simplicity take last of g or occurrence flags and
	 *        ignore multiple p flags. need to fix that */
	for (; s < p; s++) {
		if (isdigit(*s)) {
			c->u.s.occurrence = stol(s, &s);
		} else {
			switch (*s) {
			case 'g': c->u.s.occurrence = 0; break;
			case 'p': c->u.s.p = 1;          break;
			case 'w':
				/* must be last flag, take everything up to newline/semicolon
				 * s == p after this */
				s = get_w_arg(&buf, s);
				c->u.s.file = buf.u.file;
				break;
			}
		}
	}
	return p;
}

void
free_s_arg(Cmd *c)
{
	if (c->u.s.re) {
		regfree(c->u.s.re);
		free(c->u.s.re);
	}
	free(c->u.s.repl.str);
}

/* see get_r_arg notes */
char *
get_w_arg(Cmd *c, char *s)
{
	char *p = semicolon_arg(s = chomp(s));
	Wfile *w, **wp;

	if (p == s)
		leprintf("no file name");

	/* man -Wsigncompare is annoying */
	for (wp = (Wfile **)wfiles.data; (size_t)(wp - (Wfile **)wfiles.data) < wfiles.size; wp++) {
		if (strlen((*wp)->path) == (size_t)(p - s) && !strncmp(s, (*wp)->path, p - s)) {
			c->u.file = (*wp)->file;
			return p;
		}
	}

	w = emalloc(sizeof(*w));
	w->path = estrndup(s, p - s);

	if (!(w->file = fopen(w->path, "w")))
		leprintf("fopen failed");

	c->u.file = w->file;

	push(&wfiles, w);
	return p;
}

char *
get_y_arg(Cmd *c, char *s)
{
	Rune delim;
	char *p = s + strlen(s);
	size_t rlen = echarntorune(&delim, s, p - s);
	size_t nrunes1, nrunes2;

	c->u.y.set1 = c->u.y.set2 = NULL;

	s += rlen;
	p = find_delim(s, delim, 0);
	p -= escapes(s, p, delim, 1);
	nrunes1 = utfnlen(s, p - s);
	c->u.y.set1 = strtorunes(s, nrunes1);

	s = p + rlen;
	p = find_delim(s, delim, 0);
	p -= escapes(s, p, delim, 1);
	nrunes2 = utfnlen(s, p - s);

	if (nrunes1 != nrunes2)
		leprintf("different set lengths");

	c->u.y.set2 = strtorunes(s, utfnlen(s, p - s));

	return p + rlen;
}

void
free_y_arg(Cmd *c)
{
	free(c->u.y.set1);
	free(c->u.y.set2);
}

/* see get_bt_arg notes */
char *
get_colon_arg(Cmd *c, char *s)
{
	char *p = semicolon_arg(s = chomp(s));

	if (p == s)
		leprintf("no label name");

	c->u.label = estrndup(s, p - s);
	push(&labels, (void *)(c - prog));
	return p;
}

char *
get_lbrace_arg(Cmd *c, char *s)
{
	push(&braces, (void *)(c - prog));
	return s;
}

char *
get_rbrace_arg(Cmd *c, char *s)
{
	Cmd *lbrace;

	if (!braces.size)
		leprintf("extra }");

	lbrace = prog + (ptrdiff_t)pop(&braces);
	lbrace->u.offset = c - prog;
	return s;
}

/* s points to beginning of an argument that may be semicolon terminated
 * return pointer to semicolon or nul byte after string
 * or closing brace as to not force ; before }
 * FIXME: decide whether or not to eat trailing whitespace for arguments that
 *        we allow semicolon/brace termination that POSIX doesn't
 *        b, r, t, w, :
 *        POSIX says trailing whitespace is part of label name, file name, etc.
 *        we should probably eat it
 */
char *
semicolon_arg(char *s)
{
	char *p = strpbrk(s, ";}");
	if (!p)
		p = s + strlen(s);
	return p;
}

void
run(void)
{
	lineno = 0;
	if (braces.size)
		leprintf("extra {");

	/* genbuf has already been initialized, patt will be in new_line
	 * (or we'll halt) */
	stracpy(&hold, "");

	insert_labels();
	next_file();
	new_line();

	for (pc = prog; !gflags.halt; pc++)
		pc->fninfo->fn(pc);
}

/* return true if we are in range for c, set c->in_match appropriately */
int
in_range(Cmd *c)
{
	if (match_addr(&c->range.beg)) {
		if (c->range.naddr == 2) {
			if (c->range.end.type == LINE && c->range.end.u.lineno <= lineno)
				c->in_match = 0;
			else
				c->in_match = 1;
		}
		return !c->negate;
	}
	if (c->in_match && match_addr(&c->range.end)) {
		c->in_match = 0;
		return !c->negate;
	}
	return c->in_match ^ c->negate;
}

/* return true if addr matches current line */
int
match_addr(Addr *a)
{
	switch (a->type) {
	default:
	case IGNORE: return 0;
	case EVERY: return 1;
	case LINE: return lineno == a->u.lineno;
	case LAST:
		while (is_eof(file) && !next_file())
			;
		return !file;
	case REGEX:
		lastre = a->u.re;
		return !regexec(a->u.re, patt.str, 0, NULL, 0);
	case LASTRE:
		if (!lastre)
			leprintf("no previous regex");
		return !regexec(lastre, patt.str, 0, NULL, 0);
	}
}

/* move to next input file
 * stdin if first call and no files
 * return 0 for success and 1 for no more files
 */
int
next_file(void)
{
	static unsigned char first = 1;

	if (file == stdin)
		clearerr(file);
	else if (file && fclose(file))
		weprintf("fclose failed\n");
	file = NULL;

	do {
		if (!*files) {
			if (first) /* given no files, default to stdin */
				file = stdin;
			/* else we've used all our files, leave file = NULL */
		} else if (!strcmp(*files, "-")) {
			file = stdin;
			files++;
		} else if (!(file = fopen(*files++, "r"))) {
			/* warn this file didn't open, but move on to next */
			weprintf("fopen failed\n");
		}
	} while (!file && *files);
	first = 0;
	return !file;
}

/* test if stream is at EOF */
int
is_eof(FILE *f)
{
	int c;

	if (!f || feof(f))
		return 1;

	c = fgetc(f);
	if (c == EOF && ferror(f))
		eprintf("fgetc failed\n");
	if (c != EOF && ungetc(c, f) == EOF)
		eprintf("ungetc failed\n");

	return c == EOF;
}

/* perform writes that were scheduled
 * for aci this is check_puts(string, stdout)
 * for r this is write_file(path, stdout)
 */
void
do_writes(void)
{
	Cmd *c;
	size_t i;

	for (i = 0; i < writes.size; i++) {
		c = writes.data[i];
		c->u.acir.print(c->u.acir.str.str, stdout);
	}
	writes.size = 0;
}

/* used for r's u.acir.print()
 * FIXME: something like util's concat() would be better
 */
void
write_file(char *path, FILE *out)
{
	FILE *in = fopen(path, "r");
	if (!in) /* no file is treated as empty file */
		return;

	while (read_line(in, &genbuf) != EOF)
		check_puts(genbuf.str, out);

	if (fclose(in))
		weprintf("fclose failed\n");
}

void
check_puts(char *s, FILE *f)
{
	if (s && fputs(s, f) == EOF)
		eprintf("fputs failed\n");
	if (fputs("\n", f) == EOF)
		eprintf("fputs failed\n");
}

/* iterate from beg to end updating ranges so we don't miss any commands
 * e.g. sed -n '1d;1,3p' should still print lines 2 and 3
 */
void
update_ranges(Cmd *beg, Cmd *end)
{
	while (beg < end)
		in_range(beg++);
}

/*
 * Sed functions
 */
void
cmd_a(Cmd *c)
{
	if (in_range(c))
		push(&writes, c);
}

void
cmd_b(Cmd *c)
{
	if (!in_range(c))
		return;

	/* if we jump backwards update to end, otherwise update to destination */
	update_ranges(c + 1, c->u.jump > c ? c->u.jump : prog + pcap);
	pc = c->u.jump;
}

void
cmd_c(Cmd *c)
{
	if (!in_range(c))
		return;

	/* write the text on the last line of the match */
	if (!c->in_match)
		check_puts(c->u.acir.str.str, stdout);
	/* otherwise start the next cycle without printing pattern space
	 * effectively deleting the text */
	new_next();
}

void
cmd_d(Cmd *c)
{
	if (!in_range(c))
		return;

	new_next();
}

void
cmd_D(Cmd *c)
{
	char *p;

	if (!in_range(c))
		return;

	if ((p = strchr(patt.str, '\n'))) {
		p++;
		memmove(patt.str, p, strlen(p) + 1);
		old_next();
	} else {
		new_next();
	}
}

void
cmd_g(Cmd *c)
{
	if (in_range(c))
		stracpy(&patt, hold.str);
}

void
cmd_G(Cmd *c)
{
	if (!in_range(c))
		return;

	stracat(&patt, "\n");
	stracat(&patt, hold.str);
}

void
cmd_h(Cmd *c)
{
	if (in_range(c))
		stracpy(&hold, patt.str);
}

void
cmd_H(Cmd *c)
{
	if (!in_range(c))
		return;

	stracat(&hold, "\n");
	stracat(&hold, patt.str);
}

void
cmd_i(Cmd *c)
{
	if (in_range(c))
		check_puts(c->u.acir.str.str, stdout);
}

/* I think it makes sense to print invalid UTF-8 sequences in octal to satisfy
 * the "visually unambiguous form" sed(1p)
 */
void
cmd_l(Cmd *c)
{
	Rune   r;
	char  *p, *end;
	size_t rlen;

	char *escapes[] = { /* FIXME: 7 entries and search instead of 127 */
		['\\'] = "\\\\", ['\a'] = "\\a", ['\b'] = "\\b",
		['\f'] = "\\f" , ['\r'] = "\\r", ['\t'] = "\\t",
		['\v'] = "\\v" , [0x7f] = NULL, /* fill out the table */
	};

	if (!in_range(c))
		return;

	/* FIXME: line wrapping. sed(1p) says "length at which folding occurs is
	 * unspecified, but should be appropraite for the output device"
	 * just wrap at 80 Runes?
	 */
	for (p = patt.str, end = p + strlen(p); p < end; p += rlen) {
		if (isascii(*p) && escapes[(unsigned)*p]) {
			printf("%s", escapes[(unsigned)*p]);
			rlen = 1;
		} else if (!(rlen = charntorune(&r, p, end - p))) {
		/* ran out of chars, print the bytes of the short sequence */
			for (; p < end; p++)
				printf("\\%03hho", (unsigned char)*p);
			break;
		} else if (r == Runeerror) {
			for (; rlen; rlen--, p++)
				printf("\\%03hho", (unsigned char)*p);
		} else {
			while (fwrite(p, rlen, 1, stdout) < 1 && errno == EINTR)
				;
			if (ferror(stdout))
				eprintf("fwrite failed\n");
		}
	}
	check_puts("$", stdout);
}

void
cmd_n(Cmd *c)
{
	if (!in_range(c))
		return;

	if (!gflags.n)
		check_puts(patt.str, stdout);
	do_writes();
	new_line();
}

void
cmd_N(Cmd *c)
{
	if (!in_range(c))
		return;
	do_writes();
	app_line();
}

void
cmd_p(Cmd *c)
{
	if (in_range(c))
		check_puts(patt.str, stdout);
}

void
cmd_P(Cmd *c)
{
	char *p;

	if (!in_range(c))
		return;

	if ((p = strchr(patt.str, '\n')))
		*p = '\0';

	check_puts(patt.str, stdout);

	if (p)
		*p = '\n';
}

void
cmd_q(Cmd *c)
{
	if (!in_range(c))
		return;

	if (!gflags.n)
		check_puts(patt.str, stdout);
	do_writes();
	gflags.halt = 1;
}

void
cmd_r(Cmd *c)
{
	if (in_range(c))
		push(&writes, c);
}

void
cmd_s(Cmd *c)
{
	if (!in_range(c))
		return;

	if (!c->u.s.re && !lastre)
		leprintf("no previous regex");

	regex_t *re = c->u.s.re ? c->u.s.re : lastre;
	regmatch_t pmatch[re->re_nsub + 1];
	unsigned matches = 0, last_empty = 1, qflag = 0, cflags = 0;
	char *s = patt.str;
	String tmp;

	lastre = re;
	*genbuf.str = '\0';

	while (!qflag && !regexec(re, s, LEN(pmatch), pmatch, cflags)) {
		cflags = REG_NOTBOL; /* match against beginning of line first time, but not again */
		if (!*s) /* match against empty string first time, but not again */
			qflag = 1;

		/* don't substitute if last match was not empty but this one is.
		 * s_a*_._g
		 * foobar -> .f.o.o.b.r.
		 */
		if ((last_empty || pmatch[0].rm_eo) &&
		    (++matches == c->u.s.occurrence || !c->u.s.occurrence)) {
			char *p;
			size_t len;

			/* copy over everything before the match */
			strnacat(&genbuf, s, pmatch[0].rm_so);

			/* copy over replacement text, taking into account &, backreferences, and \ escapes */
			for (p = c->u.s.repl.str, len = strcspn(p, "\\&"); *p; len = strcspn(++p, "\\&")) {
				strnacat(&genbuf, p, len);
				p += len;
				switch (*p) {
				default: leprintf("this shouldn't be possible");
				case '\0':
					/* we're at the end, back up one so the ++p will put us on
					 * the null byte to break out of the loop */
					--p;
					break;
				case '&':
					strnacat(&genbuf, s + pmatch[0].rm_so, pmatch[0].rm_eo - pmatch[0].rm_so);
					break;
				case '\\':
					if (isdigit(*++p)) { /* backreference */
						/* only need to check here if using lastre, otherwise we checked when building */
						if (!c->u.s.re && (size_t)(*p - '0') > re->re_nsub)
							leprintf("back reference number greater than number of groups");
						regmatch_t *rm = &pmatch[*p - '0'];
						strnacat(&genbuf, s + rm->rm_so, rm->rm_eo - rm->rm_so);
					} else { /* character after backslash taken literally (well one byte, but it works) */
						strnacat(&genbuf, p, 1);
					}
					break;
				}
			}
		} else {
			/* not replacing, copy over everything up to and including the match */
			strnacat(&genbuf, s, pmatch[0].rm_eo);
		}

		if (!pmatch[0].rm_eo) { /* empty match, advance one rune and add it to output */
			Rune r;
			char *end = s + strlen(s);
			size_t rlen = charntorune(&r, s, end - s);


			if (!rlen) { /* ran out of bytes, copy short sequence */
				stracat(&genbuf, s);
				s = end;
			} else { /* copy whether or not it's a good rune */
				strnacat(&genbuf, s, rlen);
				s += rlen;
			}
		}
		last_empty = !pmatch[0].rm_eo;
		s += pmatch[0].rm_eo;
	}

	if (!(matches && matches >= c->u.s.occurrence)) /* no replacement */
		return;

	gflags.s = 1;

	stracat(&genbuf, s);

	tmp    = patt;
	patt   = genbuf;
	genbuf = tmp;

	if (c->u.s.p)
		check_puts(patt.str, stdout);
	if (c->u.s.file)
		check_puts(patt.str, c->u.s.file);
}

void
cmd_t(Cmd *c)
{
	if (!in_range(c) || !gflags.s)
		return;

	/* if we jump backwards update to end, otherwise update to destination */
	update_ranges(c + 1, c->u.jump > c ? c->u.jump : prog + pcap);
	pc = c->u.jump;
	gflags.s = 0;
}

void
cmd_w(Cmd *c)
{
	if (in_range(c))
		check_puts(patt.str, c->u.file);
}

void
cmd_x(Cmd *c)
{
	String tmp;

	if (!in_range(c))
		return;

	tmp  = patt;
	patt = hold;
	hold = tmp;
}

void
cmd_y(Cmd *c)
{
	String tmp;
	Rune r, *rp;
	size_t rlen;
	char *s, *end;

	if (!in_range(c))
		return;

	*genbuf.str = '\0';
	for (s = patt.str, end = s + strlen(s); *s; s += rlen) {
		if (!(rlen = charntorune(&r, s, end - s))) { /* ran out of chars, copy rest */
			stracat(&genbuf, s);
			break;
		} else if (r == Runeerror) { /* bad UTF-8 sequence, copy bytes */
			strnacat(&genbuf, s, rlen);
		} else {
			for (rp = c->u.y.set1; *rp; rp++)
				if (*rp == r)
					break;
			if (*rp) { /* found r in set1, replace with Rune from set2 */
				size_t n;
				char buf[UTFmax];
				n = runetochar(buf, c->u.y.set2 + (rp - c->u.y.set1));
				strnacat(&genbuf, buf, n);
			} else {
				strnacat(&genbuf, s, rlen);
			}
		}
	}
	tmp    = patt;
	patt   = genbuf;
	genbuf = tmp;
}

void
cmd_colon(Cmd *c)
{
}

void
cmd_equal(Cmd *c)
{
	if (in_range(c))
		printf("%zu\n", lineno);
}

void
cmd_lbrace(Cmd *c)
{
	Cmd *jump;

	if (in_range(c))
		return;

	/* update ranges on all commands we skip */
	jump = prog + c->u.offset;
	update_ranges(c + 1, jump);
	pc = jump;
}

void
cmd_rbrace(Cmd *c)
{
}

/* not actually a sed function, but acts like one, put in last spot of script */
void
cmd_last(Cmd *c)
{
	if (!gflags.n)
		check_puts(patt.str, stdout);
	do_writes();
	new_next();
}

/*
 * Actions
 */

/* read new line, continue current cycle */
void
new_line(void)
{
	while (read_line(file, &patt) == EOF) {
		if (next_file()) {
			gflags.halt = 1;
			return;
		}
	}
	gflags.s = 0;
	lineno++;
}

/* append new line, continue current cycle
 * FIXME: used for N, POSIX specifies do not print pattern space when out of
 *        input, but GNU does so busybox does as well. Currently we don't.
 *        Should we?
 */
void app_line(void)
{
	while (read_line(file, &genbuf) == EOF) {
		if (next_file()) {
			gflags.halt = 1;
			return;
		}
	}

	stracat(&patt, "\n");
	stracat(&patt, genbuf.str);
	gflags.s = 0;
	lineno++;
}

/* read new line, start new cycle */
void
new_next(void)
{
	update_ranges(pc + 1, prog + pcap);
	new_line();
	pc = prog - 1;
}

/* keep old pattern space, start new cycle */
void
old_next(void)
{
	update_ranges(pc + 1, prog + pcap);
	pc = prog - 1;
}

int
main(int argc, char *argv[])
{
	char *arg;
	int script = 0;

	ARGBEGIN {
	case 'n':
		gflags.n = 1;
		break;
	case 'e':
		arg = EARGF(usage());
		compile(arg, 0);
		script = 1;
		break;
	case 'f':
		arg = EARGF(usage());
		compile(arg, 1);
		script = 1;
		break;
	default : usage();
	} ARGEND;

	/* no script to run */
	if (!script && !argc)
		usage();

	/* no script yet, next argument is script */
	if (!script)
		compile(*argv++, 0);

	/* shrink/grow memory to fit and add our last instruction */
	resize((void **)&prog, &pcap, sizeof(*prog), pc - prog + 1, NULL);
	pc = prog + pcap - 1;
	pc->fninfo = &(Fninfo){ cmd_last, NULL, NULL, 0 };

	files = argv;
	run();
	return 0;
}

/*
 * chd.c: A hexdump-like utility for Unicode characters.
 *
 * Copyright (C) 2013-2017 Brian Raiter <breadbox@muppetlabs.com>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>
#include <getopt.h>

static int const ctlpics = 0x2400;	/* Unicode control pics start */
static int const replacechar = 0xFFFD;	/* Unicode replacement character */
static int const rawbyte = 0x10000000;	/* flag indicating a raw byte value */

/* Online help.
 */
static char const *yowzitch =
    "Usage: chd [OPTIONS] [FILENAME ...]\n"
    "Output a representation of the contents of FILENAME as character\n"
    "codepoints, similar to xxd but Unicode-aware. With multiple arguments,\n"
    "the files' contents are concatenated together. With no arguments, or\n"
    "when FILENAME is -, read from standard input.\n"
    "\n"
    "  -c, --count=N         Display N characters per line [default=8]\n"
    "  -i, --ignore          Treat invalid characters as individual bytes\n"
    "  -s, --start=N         Start N characters after start of input\n"
    "  -l, --limit=N         Stop after N characters of input\n"
    "  -r, --reverse         Reverse operation: convert dump output to chars\n"
    "      --help            Display this help and exit\n"
    "      --version         Display version information and exit\n";

/* Version information.
 */
static char const *vourzhon =
    "chd: v1.1\n"
    "Copyright (C) 2013-2017 by Brian Raiter <breadbox@muppetlabs.com>\n"
    "This is free software; you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n";

/* The program's input file state and user-controlled settings.
 */
typedef struct state {
    int startoffset;	/* skip over this many chars of input at start */
    int maxinputlen;	/* stop after this many chars of input */
    char **filenames;	/* NULL-terminated list of input filenames */
    FILE *currentfile;	/* handle to the currently open input file */
} state;

/* Number of characters to display per line of dump output. (The
 * default value is 8, which produces output that fits comfortably on
 * an 80-column display.)
 */
static int linesize = 8;

/* If nonzero, treat invalid sequences as raw bytes (octets).
 */
static int acceptbadchars = 0;

/* The program's (eventual) exit code.
 */
static int exitcode = 0;

/*
 * Basic functions.
 */

/* Display a formatted message on stderr and exit.
 */
void die(char const *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

/* Display an error message for the current file and set the exit code.
 */
static void fail(state *s)
{
    perror(s->filenames && *s->filenames ? *s->filenames : "chd");
    exitcode = EXIT_FAILURE;
}

/* Read a small non-negative integer from a string. Exit with a
 * simple error message if the string is not a valid number, or if
 * the number hits a given upper limit.
 */
static int getn(char const *str, char const *name, int maxval)
{
    char *p;
    long n;

    if (!str || !*str)
	die("missing argument for %s", name);
    n = strtol(str, &p, 0);
    if (*p != '\0' || p == str ||
		(n == LONG_MAX && errno == ERANGE) || n < 0 || n > INT_MAX)
	die("invalid argument '%s' for %s", str, name);
    if (maxval && n > maxval)
	die("value for %s too large (maximum %d)", name, maxval);
    return n;
}

/*
 * File I/O.
 */

/* Prepare the current input file, if necessary. (Does nothing if the
 * current input file is already open and is not at the end.) Any
 * errors that occur when opening a file are reported to stderr before
 * moving on to the next input file. The return value is zero if no
 * more input files are available.
 */
static int inputinit(state *s)
{
    if (!s->currentfile) {
	if (!*s->filenames)
	    return 0;
	if (!strcmp(*s->filenames, "-")) {
	    s->currentfile = stdin;
	    *s->filenames = "stdin";
	} else {
	    s->currentfile = fopen(*s->filenames, "r");
	}
	if (!s->currentfile) {
	    fail(s);
	    ++s->filenames;
	    return inputinit(s);
	}
    }
    return 1;
}

/* Close the current input file, reporting errors if any to stderr.
 */
static void inputupdate(state *s)
{
    if (ferror(s->currentfile)) {
	fail(s);
	fclose(s->currentfile);
    } else {
	if (s->currentfile != stdin)
	    if (fclose(s->currentfile))
		fail(s);
    }
    s->currentfile = 0;
    ++s->filenames;
}

/* Get the next character from the current file. If there are no more
 * characters in the current file, open the next file in the list of
 * filenames and get a character from that. If there are no more
 * filenames, return WEOF. If an invalid byte sequence is encountered
 * and acceptbadchars is true, then a single byte is retrieved from
 * the current file, and the return value is the the value of the
 * octet ORed with the rawbyte flag.
 */
static wchar_t nextwchar(state *s)
{
    wint_t  ch;

    for (;;) {
	if (!inputinit(s))
	    return WEOF;
	ch = fgetwc(s->currentfile);
	if (ch != WEOF)
	    return ch;
	if (ferror(s->currentfile) && errno == EILSEQ && acceptbadchars) {
	    clearerr(s->currentfile);
	    return rawbyte | fgetc(s->currentfile);
	}
	inputupdate(s);
    }
}

/* Get a line of text from the current file and store it in buf.
 * Return zero if no further input is available.
 */
static int nextwline(state *s, wchar_t *buf, int buflen)
{
    for (;;) {
	if (!inputinit(s))
	    return 0;
	if (fgetws(buf, buflen, s->currentfile))
	    return 1;
	inputupdate(s);
    }
}

/*
 * Dump format functions.
 */

/* Output one line of data as a hexdump, containing up to linesize
 * characters. pos supplies the current file position.
 */
static void renderdumpline(wchar_t const *buf, int count, int pos)
{
    int i;

    wprintf(L"%08X: ", pos);
    for (i = 0 ; i < count ; ++i) {
	if (buf[i] < 256)
	    wprintf(L"    %02X", buf[i]);
	else if (buf[i] & rawbyte)
	    wprintf(L"   *%02X", buf[i] & 0xFF);
	else
	    wprintf(L"%6X", buf[i]);
    }
    wprintf(L"%*s", 6 * (linesize - count) + 5, "");
    for (i = 0 ; i < count ; ++i) {
	switch (wcwidth(buf[i])) {
	  case 2:
	    putwchar(buf[i]);
	    break;
	  case 1:
	    putwchar(buf[i]);
	    putwchar(L' ');
	    break;
	  default:
	    if (buf[i] < 0x20)
		putwchar(ctlpics + buf[i]);
	    else
		putwchar(replacechar);
	    putwchar(L' ');
	    break;
	}
    }
    putwchar(L'\n');
}

/* Parse input as a line of dumped data and output the characters
 * represented therein. Return the number of characters output. If
 * line is NULL, the function resets stdout's shift state. (Because
 * the output may need to include embedded raw bytes, wcrtomb() is
 * used to translate characters into byte sequences instead of using
 * putwchar() directly.)
 */
static int translatedumpline(wchar_t *line)
{
    static mbstate_t mbs;

    char     out[MB_CUR_MAX + 1];
    wchar_t *p;
    int      ch, i, n;

    if (!line) {
	fwrite(out, wcrtomb(out, L'\0', &mbs) - 1, 1, stdout);
	return 0;
    }

    for (p = line ; *p != L'\0' && *p != L' ' ; ++p) ;
    if (!*p)
	return 0;
    ++p;
    for (i = 0 ; i < linesize ; ++i) {
	if (swscanf(p, L"%6X", &ch) == 1) {
	    n = wcrtomb(out, ch, &mbs);
	    if (n < 0)
		n = wcrtomb(out, replacechar, &mbs);
	    fwrite(out, n, 1, stdout);
	} else if (swscanf(p, L" *%2X", &ch) == 1) {
	    n = wcrtomb(out, L'\0', &mbs);
	    out[n - 1] = ch;
	    fwrite(out, n, 1, stdout);
	} else {
	    break;
	}
	p += 6;
    }
    return i;
}

/*
 * The main program functions.
 */

/* Display hexdump lines from the given filenames until there's no
 * more input.
 */
static void dump(state *s)
{
    wchar_t line[256];
    wint_t  ch = 0;
    int     pos = 0;
    int     n;

    for (pos = 0 ; ch != WEOF && pos < s->startoffset ; ++pos)
	ch = nextwchar(s);

    while (ch != WEOF && s->maxinputlen > 0) {
	for (n = 0 ; n < linesize && s->maxinputlen > 0 ; ++n) {
	    ch = nextwchar(s);
	    if (ch == WEOF)
		break;
	    line[n] = ch;
	    --s->maxinputlen;
	}
	if (n)
	    renderdumpline(line, n, pos);
	pos += n;
    }
}

/* Input dump lines and turn them into character output.
 */
static void undump(state *s)
{
    wchar_t  *line;
    int       len;

    len = linesize * 8 + 20;
    line = malloc(len * 4);
    while (nextwline(s, line, len) && s->maxinputlen > 0)
	s->maxinputlen -= translatedumpline(line);
    translatedumpline(NULL);
    free(line);
}

/* Parse the command-line arguments and initialize the given state
 * appropriately. Invalid arguments will cause the program to
 * terminate. The return value is true if the user requested a normal
 * dump, or zero if the program should translate dump input back into
 * literal characters.
 */
static int parsecommandline(int argc, char *argv[], state *s)
{
    static char *defaultargs[] = { "-", NULL };
    static char const *optstring = "c:il:rs:";
    static struct option options[] = {
	{ "count", required_argument, NULL, 'c' },
	{ "limit", required_argument, NULL, 'l' },
	{ "start", required_argument, NULL, 's' },
	{ "ignore", no_argument, NULL, 'i' },
	{ "reverse", no_argument, NULL, 'r' },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, 'v' },
	{ 0, 0, 0, 0 }
    };

    int forward = 1;
    int ch;

    s->startoffset = 0;
    s->maxinputlen = INT_MAX;
    s->filenames = defaultargs;
    s->currentfile = NULL;

    while ((ch = getopt_long(argc, argv, optstring, options, NULL)) != EOF) {
	switch (ch) {
	  case 'l':	s->maxinputlen = getn(optarg, "limit", 0);  break;
	  case 's':	s->startoffset = getn(optarg, "start", 0);  break;
	  case 'c':	linesize = getn(optarg, "count", 255);	    break;
	  case 'i':	acceptbadchars = 1;			    break;
	  case 'r':	forward = 0;				    break;
	  case 'h':	fputs(yowzitch, stdout);		    exit(0);
	  case 'v':	fputs(vourzhon, stdout);		    exit(0);
	  default:	die("Try --help for more information.");
	}
    }
    if (optind < argc)
	s->filenames = argv + optind;

    return forward;
}

/* Main.
 */
int main(int argc, char *argv[])
{
    state s;

    setlocale(LC_ALL, "");
    if (parsecommandline(argc, argv, &s))
	dump(&s);
    else
	undump(&s);
    return exitcode;
}

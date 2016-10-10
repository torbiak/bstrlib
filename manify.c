/* manify.c defines a flex scanner that creates a directory of manpages from
 * bstrlib.txt: one for the library in general, and one for each exposed
 * function/macro.
 *
 * It doesn't try to be perfect and fails to follow a lot of manpage
 * conventions, such as having all the customary headings, bolding and
 * italicizing function names and arguments, etc.
 *
 * It's probably most useful for people wanting to look up manpages from their
 * editor, for example using Shift-K in Vim.
 */
%{
#include <bsd/string.h>
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ARRAY_SIZE(A) (sizeof(A) / sizeof(*A))

#define FILEBUFLEN 100
#define BUFLEN 5000
#define FUNC_RE "[bu][a-zA-Z0-9-]+ ?\\("

#define clean_errno() (errno == 0 ? "" : strerror(errno))
#define log_err(M, ...) fprintf(stderr, "[ERROR] %s:%d:%s:" M "\n", __FILE__, __LINE__, clean_errno(), ##__VA_ARGS__)
#define die(M, ...) do { log_err(M, ##__VA_ARGS__); abort(); } while(0);

char g_esc_buf[BUFLEN] = {0};
char g_trim_buf[BUFLEN] = {0};
char g_indent_buf[BUFLEN] = {0};
char g_buf[BUFLEN] = {0};
int g_hanging = 0;
regex_t *g_defname_re = NULL;
FILE *g_def_file = NULL;


void
compile_regexes()
{
	if (!g_defname_re) {
		g_defname_re = malloc(sizeof(regex_t));
		if (!g_defname_re) {
			die("out of memory");
		}
		int err = regcomp(g_defname_re, FUNC_RE, REG_EXTENDED);
		if (err) {
			int msglen = regerror(err, g_defname_re, NULL, 0);
			char *s = malloc(msglen);
			if (!s) die("out of memory");
			regerror(err, g_defname_re, s, msglen);
			die("%s", s);
		}
	}
}

int
isdir(char *path)
{
	int ret = 0;
	struct stat s;
	int err = stat(path, &s);
	if (err == -1) {
		if(errno == ENOENT) { // Does not exist.
			ret = 0;
		} else {
			die("stat %s", path);
		}
	} else {
		if(S_ISDIR(s.st_mode)) {
			ret = 1;
		} else {
			ret = 0;
		}
	}
	errno = 0;
	return ret;
}

FILE *
open_deffile(char *name)
{
	char dir[] = "man3";
	char path[FILEBUFLEN+1] = {};
	if (!isdir(dir)) {
		int err = mkdir(dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		if (err) die("create manpage dir");
	}
	snprintf(path, FILEBUFLEN, "%s/%s.3", dir, name);
	FILE *f = fopen(path, "w");
	if (!f) die("open manpage file");
	return f;
}

void
str2upper(char *s)
{
	for (char *c = s; *c; c++) {
		*c = toupper(*c);
	}
}

/* esc escapes backslashes and leading apostrophes (which would otherwise start
 * a troff macro.
 *
 * Note that the global buffer g_esc_buf is overwritten on each call to esc.
 * Having an INOUT parameter seems less error-prone and more encapsulated than
 * either returning the buffer or having clients use the global name for the
 * buffer.
 */
void
esc(char **s_inout)
{
	char *s = *s_inout;
	int dst = 0;
	for (int i = 0, c = s[0]; c; c = s[++i]) {
		if ((size_t) dst > ARRAY_SIZE(g_esc_buf)) die("buffer overrun on %.40s", s);
		if (c == '\\') g_esc_buf[dst++] = '\\';
		if ((c == '\'' || c == '.') && i > 0 && s[i-1] == '\n') g_esc_buf[dst++] = '\\';
		g_esc_buf[dst++] = c;
	}
	g_esc_buf[dst] = '\0';
	*s_inout = g_esc_buf;
}

/* bstart starts building a string in a global buffer using yytext. */
void
bstart()
{
	if (strlcpy(g_buf, yytext, BUFLEN) >= BUFLEN) die("buffer overrun");
}

/* bstart appends yytext to the global buffer. */
void
bappend()
{
	if (strlcat(g_buf, yytext, BUFLEN) >= BUFLEN) die("buffer overrun");
}

/* bclear clears the global buffer. */
void
bclear()
{
	g_buf[0] = '\0';
}

/* Note that the global buffer g_trim_buf is overwritten on each call to
 * trim_leading_spaces.
 */
void
trim_leading_spaces(char **s_inout)
{
	char *s = *s_inout;
	size_t src = 0, dst = 0;
	int c = s[src];
	int trimming = 1;
	while (c) {
		trimming = c == '\n' || (trimming && c == ' ');
		if (trimming && c == ' ') {
			// pass
		} else {
			g_trim_buf[dst++] = c;
			if (dst >= ARRAY_SIZE(g_trim_buf)) {
				die("trim buffer too small for: %.40s", s);
			}
		}
		c = s[++src];
	}
	g_trim_buf[dst] = '\0';
	*s_inout = g_trim_buf;
}

/* Note that the global buffer g_indent_buf is overwritten on each call to
 * indent.
 */
void
indent(char **s_inout, int spaces)
{
	char *s = *s_inout;
	size_t src = 0, dst = 0;
	int c = s[src];
	int spaceAfterNewline = 1;
	int delta = spaces;
	while (c) {
		if (delta > 0) {
			g_indent_buf[dst++] = ' ';
			delta--;
			if (dst >= ARRAY_SIZE(g_indent_buf)) goto overflow;
		} else if (spaceAfterNewline && c == ' ' && delta < 0) {
			delta++;
		} else {
			g_indent_buf[dst++] = c;
			if (dst >= ARRAY_SIZE(g_indent_buf)) goto overflow;
		}
		if (c == '\n') {
			delta = spaces;
			spaceAfterNewline = 1;
		}
		c = s[++src];
		if (c != ' ') spaceAfterNewline = 0;
	}
	g_indent_buf[dst] = '\0';
	*s_inout = g_indent_buf;
	return;
overflow:
	die("indent buffer too small for: %.40s", s);
}


// Start the main man page.
void
bstrlib_head()
{
	printf(".TH BSTRLIB 3\n");
	printf(".SH NAME\nbstrlib \\- the better string library\n");
}

void
heading(int level)
{
	char *macro = NULL;
	if (level == 1) {
		macro = ".SH";
	} else if (level > 1) {
		macro = ".SS";
	} else {
		die("bad heading level: %d", level);
	}
	if (level == 1) str2upper(yytext);
	char *nl = index(yytext, '\n');
	if (!nl) die("no newline in heading: %s", yytext);
	printf("%s %.*s", macro, (int) (nl - yytext) + 1, yytext);
}

void
par(char *s)
{
	trim_leading_spaces(&s);
	esc(&s);
	printf(".P\n%s", s);
}

void
ol()
{
	char *s = g_buf;
	if (!strlen(s)) return;
	// If the first newline is followed by spaces the paragraph should have a
	// hanging indent.
	int hanging = 1;
	char *nl = index(s, '\n');
	if (nl && (size_t) (nl + 1 - s) < strlen(s) && nl[1] != ' ') hanging = 0;
	trim_leading_spaces(&s);
	esc(&s);
	size_t label_len = strspn(s, "0123456789.)");
	if (!label_len) die("can't find ol marker: %.20s", s);
	size_t ws_len = strspn(s + label_len, " ");
	if (hanging) {
		printf(".TP\n%.*s\n%s", (int) label_len, s, s+label_len+ws_len);
	} else {
		par(s);
	}
	bclear();
}

void
ul()
{
	char *s = g_buf;
	if (!strlen(s)) return;
	esc(&s);
	trim_leading_spaces(&s);
	size_t prefix_len = strspn(s, "- ");
	printf(".TP\n-\n%s", s + prefix_len);
	bclear();
}

void
bq()
{
	char *s = g_buf;
	if (!strlen(s)) return;
	esc(&s);
	int init_indent = strspn(s, " ");
	indent(&s, 4-init_indent);
	printf("\n.EX\n%s.EE\n", s);
	bclear();
}

void
nf(char *s)
{
	esc(&s);
	printf("\n.nf\n%s.fi\n", s);
}

void
macrodesc(char *s)
{
	esc(&s);
	trim_leading_spaces(&s);
	char *tagend = index(s, '\n');
	if (!tagend) die("Probably not a compilation macro description: %s", s);
	int taglen = tagend - s;
	int desc_offset = taglen + strspn(s + taglen, " -\n");
	printf(".TP\n%.*s\n%s", taglen, s, s + desc_offset);
}

char *
func_name()
{
	regmatch_t m = {};
	int err = regexec(g_defname_re, yytext, 1, &m, 0);
	if (err) die("No match: '%s' and '%s'", FUNC_RE, yytext);
	size_t eo;
	for (eo = m.rm_eo-1; eo > 0; eo--) {
		if (yytext[eo] != '(' && yytext[eo] != ' ') break;
	}
	size_t len = eo + 1 - m.rm_so;
	char *name = malloc(len+1);
	if (!name) die("out of memory");
	strncpy(name, yytext+m.rm_so, len);
	name[len] = '\0';
	return name;
}

void
func_head()
{
	char *name = func_name();
	int len = strlen(name) + 1;
	char *upper = malloc(len);
	strlcpy(upper, name, len);
	str2upper(upper);

	char *s = yytext;
	indent(&s, -strspn(yytext, " "));
	esc(&s);

	// Don't show obvious extern qualifiers.
	char *ext = "extern ";
	char *loc = strstr(s, ext);
	if (loc) {
		s = loc + strlen(ext);
		char *nl = index(s, '\n');
		// Realign the next line, if any.
		if (strlen(nl+1) > strlen(ext)) {
			memmove(nl+1, nl+1+strlen(ext), strlen(nl+1)-strlen(ext)+1);
		}
	}

	g_def_file = open_deffile(name);
	fprintf(g_def_file, ".TH %s 3\n", upper);
	fprintf(g_def_file, ".SH NAME\n%s \\- bstrlib function\n", name);
	fprintf(g_def_file, ".SH SYNOPSIS\n.EX\n%s\n.EE\n", s);
	// Assumes some paragraphs follow...
	fprintf(g_def_file, ".SH DESCRIPTION\n");
	free(name);
	return;
}

void
func_par()
{
	char *s = yytext;
	esc(&s);
	trim_leading_spaces(&s);
	fprintf(g_def_file, ".P\n%s", s);
}

void
func_ex()
{
	char *s = yytext;
	esc(&s);
	int init_indent = strspn(s, " ");
	indent(&s, 4-init_indent);
	fprintf(g_def_file, ".br\n.EX\n%s.EE\n", s);
}

void
func_exit()
{
	int err = fclose(g_def_file);
	if (err) die("close def file");
}


%}

/* Horizontal white space */
HWS  [ \t]
NONBLANK ({HWS}*[^ \t\n].*\n)
	/* Also see the OL_MARKER cpp macro. */
OL_MARKER [).]

/* Valid C identifier characters */
CID [A-Za-z0-9_]

%x OL UL BQ FUNC_HEAD FUNC_BODY FUNC_EX UNIPARS MAKE TABLE


%%
	compile_regexes();

	/* Hack to print the main manpage's title. */
Better\ String\ library\n---------------------\n  bstrlib_head(); heading(1);

	/* Function/macro manpages. */
<INITIAL,FUNC_BODY>^\ {4}\.{5,}\n\n|The\ functions\n-{5,}\n\n|^The\ macros\n\n{NONBLANK}+\n\n  BEGIN(FUNC_HEAD);
<FUNC_BODY>={5,}\n  func_exit(); BEGIN(0);
<FUNC_HEAD>{NONBLANK}+  func_head(); BEGIN(FUNC_BODY);
	/* NONBLANK lines, followed by a nonblank line ending in a colon. */
<FUNC_BODY>{NONBLANK}*{HWS}*[^\ \t\n].*:\n\n  func_par(); BEGIN(FUNC_EX);
<FUNC_EX>{NONBLANK}+  func_ex(); BEGIN(FUNC_BODY);
<FUNC_BODY>{NONBLANK}+  func_par();
	/* Eat blank lines. */
<FUNC_BODY>\n

	/* The paragraphs after the Unicode functions heading are indented for no
	 * reason. Format them as normal paragraphs instead of as examples. */
Unicode\ functions\n-{3,}\n\n  heading(1); BEGIN(UNIPARS);
<UNIPARS>\ +\.{3,}\n\n  yyless(0); BEGIN(0);
<UNIPARS>{NONBLANK}+  par(yytext);

^.{3,}\n-{3,}\n  heading(1);
^.{3,}\n\.{3,}\n  heading(2);
	/* Eat section dividers. */
^={3,}\n
	/* Eat unneeded newlines. */
{HWS}*\n

	/* Ordered list */
<INITIAL,OL>\ *[[:digit:]]+{OL_MARKER}\ .*\n  ol(); bstart(); BEGIN(OL);
<OL>.+\n  bappend();
<OL>\n  ol(); BEGIN(0);
<OL><<EOF>>  ol(); BEGIN(0);

	/* Unordered list */
<INITIAL,UL>\ *-\ .*\n  ul(); bstart(); BEGIN(UL);
<UL>.+\n  bappend();
<UL>\n  ul(); BEGIN(0);
<UL><<EOF>>  ul(); BEGIN(0);

	/* Indented block quote */
<INITIAL>\ {4,}.*\n  bstart(); BEGIN(BQ);
<BQ>\ {4,}.*\n  bappend();
<BQ>\n  bappend();
	/* Ends on an unindented line or EOF */
<BQ>\n\ {0,3}[^\ ]  bq(); yyless(0); BEGIN(0); 
<BQ><<EOF>>  bq(); BEGIN(0);

	/* Special treatment for the Makefile example. */
BSTRDIR\ =\ .+\n{NONBLANK}+\n  bstart(); BEGIN(MAKE);
<MAKE>{NONBLANK}\t.+\n{NONBLANK}+\n  bappend();
<MAKE>{NONBLANK}+  nf(g_buf); bclear(); yyless(0); BEGIN(0);

{NONBLANK}(\ *-{3,}){2,}\ *\n{NONBLANK}*  bstart(); BEGIN(TABLE);
<TABLE>.*\ {3,}.*\n|\n  bappend();
<TABLE>{NONBLANK}  nf(g_buf); bclear(); yyless(0); BEGIN(0);

	/* Special treatment for the ACKNOWLEDGEMENTS. */
{NONBLANK}*Bjorn\ Augestad\n{NONBLANK}*  nf(yytext);

	/* Clean up the compilation macro descriptions. */
BSTRLIB_[A-Z0-9_]+\n\n{NONBLANK}+\n  macrodesc(yytext);

	/* Special treatment for the FILES section. */
{NONBLANK}?([a-zA-Z0-9_]+\.[a-z]+\ {2,}-\ .+\n)+  nf(yytext);

	/* Paragraph catchall. */
[[:^space:]]{-}[[:digit:]]{-}[-].*\n(.+\n)*  par(yytext);


%%

int
main()
{
	yylex();
}

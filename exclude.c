/* -*- c-file-style: "linux" -*-
 *
 * Copyright (C) 1996-2001 by Andrew Tridgell <tridge@samba.org>
 * Copyright (C) 1996 by Paul Mackerras
 * Copyright (C) 2002 by Martin Pool
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* a lot of this stuff was originally derived from GNU tar, although
   it has now changed so much that it is hard to tell :) */

/* include/exclude cluestick added by Martin Pool <mbp@samba.org> */

#include "rsync.h"

extern int verbose;

struct exclude_struct **exclude_list;
struct exclude_struct **local_exclude_list;
struct exclude_struct **server_exclude_list;
char *exclude_path_prefix = NULL;

/** Build an exclude structure given a exclude pattern */
static struct exclude_struct *make_exclude(const char *pattern, int pat_len,
					   int include)
{
	struct exclude_struct *ret;
	const char *cp;
	int ex_len;

	ret = new(struct exclude_struct);
	if (!ret)
		out_of_memory("make_exclude");

	memset(ret, 0, sizeof ret[0]);
	ret->include = include;

	if (exclude_path_prefix)
		ret->match_flags |= MATCHFLG_ABS_PATH;
	if (exclude_path_prefix && *pattern == '/')
		ex_len = strlen(exclude_path_prefix);
	else
		ex_len = 0;
	ret->pattern = new_array(char, ex_len + pat_len + 1);
	if (!ret->pattern)
		out_of_memory("make_exclude");
	if (ex_len)
		memcpy(ret->pattern, exclude_path_prefix, ex_len);
	strlcpy(ret->pattern + ex_len, pattern, pat_len + 1);
	pat_len += ex_len;

	if (strpbrk(ret->pattern, "*[?")) {
		ret->match_flags |= MATCHFLG_WILD;
		if (strstr(ret->pattern, "**")) {
			ret->match_flags |= MATCHFLG_WILD2;
			/* If the pattern starts with **, note that. */
			if (*ret->pattern == '*' && ret->pattern[1] == '*')
				ret->match_flags |= MATCHFLG_WILD2_PREFIX;
		}
	}

	if (pat_len > 1 && ret->pattern[pat_len-1] == '/') {
		ret->pattern[pat_len-1] = 0;
		ret->directory = 1;
	}

	for (cp = ret->pattern; (cp = strchr(cp, '/')) != NULL; cp++)
		ret->slash_cnt++;

	return ret;
}

static void free_exclude(struct exclude_struct *ex)
{
	free(ex->pattern);
	memset(ex, 0, sizeof ex[0]);
	free(ex);
}


void free_exclude_list(struct exclude_struct ***listp)
{
	struct exclude_struct **list = *listp;

	if (verbose > 2)
		rprintf(FINFO, "[%s] clearing exclude list\n", who_am_i());

	if (!list)
		return;

	while (*list)
		free_exclude(*list++);

	free(*listp);
	*listp = NULL;
}

static int check_one_exclude(char *name, struct exclude_struct *ex,
                             int name_is_dir)
{
	char *p;
	int match_start = 0;
	char *pattern = ex->pattern;

	/* If the pattern does not have any slashes AND it does not have
	 * a "**" (which could match a slash), then we just match the
	 * name portion of the path. */
	if (!ex->slash_cnt && !(ex->match_flags & MATCHFLG_WILD2)) {
		if ((p = strrchr(name,'/')) != NULL)
			name = p+1;
	}
	else if ((ex->match_flags & MATCHFLG_ABS_PATH) && *name != '/') {
		static char full_name[MAXPATHLEN];
		extern char curr_dir[];
		int plus = curr_dir[1] == '\0'? 1 : 0;
		pathjoin(full_name, sizeof full_name, curr_dir+plus, name);
		name = full_name;
	}

	if (!name[0]) return 0;

	if (ex->directory && !name_is_dir) return 0;

	if (*pattern == '/') {
		match_start = 1;
		pattern++;
		if (*name == '/')
			name++;
	}

	if (ex->match_flags & MATCHFLG_WILD) {
		/* A non-anchored match with an infix slash and no "**"
		 * needs to match the last slash_cnt+1 name elements. */
		if (!match_start && ex->slash_cnt &&
		    !(ex->match_flags & MATCHFLG_WILD2)) {
			int cnt = ex->slash_cnt + 1;
			for (p = name + strlen(name) - 1; p >= name; p--) {
				if (*p == '/' && !--cnt)
					break;
			}
			name = p+1;
		}
		if (wildmatch(pattern, name))
			return 1;
		if (ex->match_flags & MATCHFLG_WILD2_PREFIX) {
			/* If the **-prefixed pattern has a '/' as the next
			 * character, then try to match the rest of the
			 * pattern at the root. */
			if (pattern[2] == '/' && wildmatch(pattern+3, name))
				return 1;
		}
		else if (!match_start && ex->match_flags & MATCHFLG_WILD2) {
			/* A non-anchored match with an infix or trailing "**"
			 * (but not a prefixed "**") needs to try matching
			 * after every slash. */
			while ((name = strchr(name, '/')) != NULL) {
				name++;
				if (wildmatch(pattern, name))
					return 1;
			}
		}
	} else if (match_start) {
		if (strcmp(name,pattern) == 0)
			return 1;
	} else {
		int l1 = strlen(name);
		int l2 = strlen(pattern);
		if (l2 <= l1 &&
		    strcmp(name+(l1-l2),pattern) == 0 &&
		    (l1==l2 || name[l1-(l2+1)] == '/')) {
			return 1;
		}
	}

	return 0;
}


static void report_exclude_result(char const *name,
                                  struct exclude_struct const *ent,
                                  int name_is_dir)
{
	/* If a trailing slash is present to match only directories,
	 * then it is stripped out by make_exclude.  So as a special
	 * case we add it back in here. */

	if (verbose >= 2) {
		rprintf(FINFO, "[%s] %scluding %s %s because of pattern %s%s\n",
			who_am_i(), ent->include ? "in" : "ex",
			name_is_dir ? "directory" : "file", name, ent->pattern,
			ent->directory ? "/" : "");
	}
}


/*
 * Return true if file NAME is defined to be excluded by either
 * LOCAL_EXCLUDE_LIST or the globals EXCLUDE_LIST.
 */
int check_exclude(struct exclude_struct **list, char *name, int name_is_dir)
{
	struct exclude_struct *ent;

	while ((ent = *list++) != NULL) {
		if (check_one_exclude(name, ent, name_is_dir)) {
			report_exclude_result(name, ent, name_is_dir);
			return !ent->include;
		}
	}

	return 0;
}


/* Get the next include/exclude arg from the string.  The token will not
 * be '\0' terminated, so use the returned length to limit the string.
 * Also, be sure to add this length to the returned pointer before passing
 * it back to ask for the next token.  This routine will not split off a
 * prefix of "+ " or "- " unless xflags contains XFLG_NO_PREFIXES.
 */
static const char *get_exclude_tok(const char *p, int *len_ptr, int xflags)
{
	const unsigned char *s, *t;

	/* Skip over any initial spaces */
	for (s = (unsigned char *)p; isspace(*s); s++) {}

	/* Remember the beginning of the token. */
	t = s;

	/* Do we have a token to parse? */
	if (*s) {
		/* Is this a '+' or '-' followed by a space (not whitespace)? */
		if (!(xflags & XFLG_NO_PREFIXES)
		    && (*s == '+' || *s == '-') && s[1] == ' ')
			s += 2;

		/* Skip to the next space or the end of the string */
		while (!isspace(*s) && *s != '\0')
			s++;
	}

	*len_ptr = s - t;
	return (const char *)t;
}


void add_exclude(struct exclude_struct ***listp, const char *pattern, int xflags)
{
	struct exclude_struct **list = *listp;
	int add_cnt, pat_len, list_len = 0;
	const char *cp;

	if (!pattern)
		return;

	if (xflags & XFLG_WORD_SPLIT) {
		/* Count how many tokens we need to add.  Also looks for
		 * the special "!" token, which clears the list up through
		 * that token. */
		for (add_cnt = 0, cp = pattern; ; cp += pat_len, add_cnt++) {
			cp = get_exclude_tok(cp, &pat_len, xflags);
			if (!pat_len)
				break;
			if (pat_len == 1 && *cp == '!') {
				free_exclude_list(listp);
				add_cnt = -1; /* Will increment to 0. */
				pattern = cp + 1;
			}
		}
		if (!add_cnt)
			return;
		cp = get_exclude_tok(pattern, &pat_len, xflags);
	} else {
		add_cnt = 1;
		cp = pattern;
		pat_len = strlen(pattern);

		if (pat_len == 1 && *cp == '!') {
			free_exclude_list(listp);
			return;
		}
	}

	if (list)
		for ( ; list[list_len]; list_len++) {}

	list = *listp = realloc_array(list, struct exclude_struct *,
				      list_len + add_cnt + 1);
	if (!list)
		out_of_memory("add_exclude");

	while (pat_len) {
		int incl = xflags & XFLG_DEF_INCLUDE;
		if (!(xflags & XFLG_NO_PREFIXES)
		    && (*cp == '-' || *cp == '+')
		    && cp[1] == ' ') {
			incl = *cp == '+';
			cp += 2;
			pat_len -= 2;
		}
		list[list_len++] = make_exclude(cp, pat_len, incl);

		if (verbose > 2) {
			rprintf(FINFO, "[%s] add_exclude(%s,%s)\n",
				who_am_i(), cp,
				incl ? "include" : "exclude");
		}
		cp += pat_len;
		cp = get_exclude_tok(cp, &pat_len, xflags);
	}

	list[list_len] = NULL;
}


void add_exclude_file(struct exclude_struct ***listp, const char *fname,
		      int xflags)
{
	FILE *fp;
	char line[MAXPATHLEN];
	char *eob = line + MAXPATHLEN - 1;
	extern int eol_nulls;

	if (!fname || !*fname)
		return;

	if (*fname != '-' || fname[1])
		fp = fopen(fname, "rb");
	else
		fp = stdin;
	if (!fp) {
		if (xflags & XFLG_FATAL_ERRORS) {
			rsyserr(FERROR, errno,
				"failed to open %s file %s",
				xflags & XFLG_DEF_INCLUDE ? "include" : "exclude",
				fname);
			exit_cleanup(RERR_FILEIO);
		}
		return;
	}

	while (1) {
		char *s = line;
		int ch;
		while (1) {
			if ((ch = getc(fp)) == EOF) {
				if (ferror(fp) && errno == EINTR)
					continue;
				break;
			}
			if (eol_nulls? !ch : (ch == '\n' || ch == '\r'))
				break;
			if (s < eob)
				*s++ = ch;
		}
		*s = '\0';
		if (*line && *line != ';' && *line != '#') {
			/* Skip lines starting with semicolon or pound.
			 * It probably wouldn't cause any harm to not skip
			 * them but there's no need to save them. */
			add_exclude(listp, line, xflags);
		}
		if (ch == EOF)
			break;
	}
	fclose(fp);
}


void send_exclude_list(int f)
{
	int i;
	extern int list_only, recurse;

	/* This is a complete hack - blame Rusty.
	 *
	 * FIXME: This pattern shows up in the output of
	 * report_exclude_result(), which is not ideal. */
	if (list_only && !recurse)
		add_exclude(&exclude_list, "/*/*", 0);

	if (!exclude_list) {
		write_int(f, 0);
		return;
	}

	for (i = 0; exclude_list[i]; i++) {
		unsigned int l;
		char p[MAXPATHLEN+1];

		l = strlcpy(p, exclude_list[i]->pattern, sizeof p);
		if (l == 0 || l >= MAXPATHLEN)
			continue;
		if (exclude_list[i]->directory) {
			p[l++] = '/';
			p[l] = '\0';
		}

		if (exclude_list[i]->include) {
			write_int(f, l + 2);
			write_buf(f, "+ ", 2);
		} else if ((*p == '-' || *p == '+') && p[1] == ' ') {
			write_int(f, l + 2);
			write_buf(f, "- ", 2);
		} else
			write_int(f, l);
		write_buf(f, p, l);
	}

	write_int(f, 0);
}


void recv_exclude_list(int f)
{
	char line[MAXPATHLEN+1]; /* Allows a trailing slash on a max-len dir */
	unsigned int l;

	while ((l = read_int(f)) != 0) {
		if (l >= sizeof line)
			overflow("recv_exclude_list");
		read_sbuf(f, line, l);
		add_exclude(&exclude_list, line, 0);
	}
}


static char default_cvsignore[] = 
	/* These default ignored items come from the CVS manual. */
	"RCS SCCS CVS CVS.adm RCSLOG cvslog.* tags TAGS"
	" .make.state .nse_depinfo *~ #* .#* ,* _$* *$"
	" *.old *.bak *.BAK *.orig *.rej .del-*"
	" *.a *.olb *.o *.obj *.so *.exe"
	" *.Z *.elc *.ln core"
	/* The rest we added to suit ourself. */
	" .svn/";

void add_cvs_excludes(void)
{
	char fname[MAXPATHLEN];
	char *p;

	add_exclude(&exclude_list, default_cvsignore,
		    XFLG_WORD_SPLIT | XFLG_NO_PREFIXES);

	if ((p = getenv("HOME"))
	    && pathjoin(fname, sizeof fname, p, ".cvsignore") < sizeof fname) {
		add_exclude_file(&exclude_list, fname,
				 XFLG_WORD_SPLIT | XFLG_NO_PREFIXES);
	}

	add_exclude(&exclude_list, getenv("CVSIGNORE"),
		    XFLG_WORD_SPLIT | XFLG_NO_PREFIXES);
}

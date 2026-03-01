#define _GNU_SOURCE

#include "exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Parse a .desktop Exec string into argv array.
 * - Handles quoting and escaping.
 * - Returns heap-allocated argv array.
 * - argc_out receives argument count.
 */
static char **parse_exec(const char *exec, int *argc_out);

/*
 * Expand Desktop Entry field codes according to spec.
 * - Modifies argv/argc in-place.
 * - Returns 0 on success, -1 on invalid field code.
 */
static int expand_field_codes(DesktopEntry *app, char ***argvp, int *argcp);

static char **
parse_exec(const char *exec, int *argc_out)
{
	char **argv = NULL;
	int argc = 0;
	const char *p = exec;

	while (*p) {
		while (*p == ' ')
			p++;

		if (!*p)
			break;

		char *arg = NULL;

		if (*p == '"') {
			p++;
			// const char *start = p;

			char *buf = malloc(strlen(p) + 1);
			int bi = 0;

			while (*p && *p != '"') {
				if (*p == '\\') {
					p++;
					if (*p)
						buf[bi++] = *p++;
				} else {
					buf[bi++] = *p++;
				}
			}

			buf[bi] = 0;
			arg = buf;

			if (*p == '"')
				p++;
		} else {
			const char *start = p;
			while (*p && *p != ' ')
				p++;

			arg = strndup(start, p - start);
		}

		argv = realloc(argv, sizeof(char *) * (argc + 1));
		argv[argc++] = arg;
	}

	*argc_out = argc;
	return argv;
}

static int
expand_field_codes(DesktopEntry *app, char ***argvp, int *argcp)
{
	char **argv = *argvp;
	int argc = *argcp;

	char **newv = NULL;
	int newc = 0;

	for (int i = 0; i < argc; i++) {
		char *arg = argv[i];

		if (strcmp(arg, "%f") == 0 || strcmp(arg, "%F") == 0 ||
			strcmp(arg, "%u") == 0 || strcmp(arg, "%U") == 0) {
			continue; // ignore (no file support)
		}

		if (strcmp(arg, "%i") == 0) {
			if (app->icon) {
				newv = realloc(newv,
					sizeof(char *) * (newc + 2));
				newv[newc++] = strdup("--icon");
				newv[newc++] = strdup(app->icon);
			}
			continue;
		}

		if (strcmp(arg, "%c") == 0) {
			newv = realloc(newv, sizeof(char *) * (newc + 1));
			newv[newc++] = strdup(app->name);
			continue;
		}

		if (strcmp(arg, "%k") == 0) {
			if (app->exec) {
				newv = realloc(newv,
					sizeof(char *) * (newc + 1));
				newv[newc++] = strdup(app->exec);
			}
			continue;
		}

		// Handle %% → %
		if (strstr(arg, "%%")) {
			char *tmp = strdup(arg);
			char *pos;
			while ((pos = strstr(tmp, "%%")))
				memmove(pos, pos + 1, strlen(pos));
			newv = realloc(newv, sizeof(char *) * (newc + 1));
			newv[newc++] = tmp;
			continue;
		}

		// Unknown field code?
		if (arg[0] == '%' && strlen(arg) == 2) {
			fprintf(stderr, "Invalid field code: %s\n", arg);
			return -1;
		}

		newv = realloc(newv, sizeof(char *) * (newc + 1));
		newv[newc++] = strdup(arg);
	}

	newv = realloc(newv, sizeof(char *) * (newc + 1));
	newv[newc] = NULL;

	*argvp = newv;
	*argcp = newc;

	return 0;
}

void
launch_app(DesktopEntry *app)
{
	if (verbose)
		printf("[DBG] -> launching app %s: %s\n", app->name, app->exec);

	if (!app->exec)
		return;

	int argc;
	char **argv = parse_exec(app->exec, &argc);

	if (!argv)
		return;

	if (expand_field_codes(app, &argv, &argc) < 0)
		return;

	pid_t pid = fork();
	if (pid == 0) {
		if (app->terminal) {
			const char *term = getenv("TERM");

			if (!term) {
				term = DEFAULT_TERMINAL;
				if (verbose)
					printf("[DBG] Cannot get the terminal, using the default one");
			}

			/*
			 * Build:
			 * terminal -e program arg1 arg2 ...
			 */
			char **term_argv = malloc(sizeof(char *) * (argc + 3));

			term_argv[0] = (char *)term;
			term_argv[1] = "-e";

			if (verbose >= 2)
				printf("[DBG²] -> launching app %s: %s %s",
					app->name, term_argv[0], term_argv[1]);
			for (int i = 0; i < argc; i++) {
				if (verbose >= 2)
					printf(" %s", argv[i]);
				term_argv[i + 2] = argv[i];
			}
			if (verbose >= 2)
				printf("\n");
			term_argv[argc + 2] = NULL;
			execvp(term, term_argv);
			perror("execvp terminal");
			_exit(1);
		} else {
			if (verbose >= 2)
				printf("[DBG²] -> launching app %s: %s\n",
					app->name, argv[0]);
			execvp(argv[0], argv);
			perror("execvp");
			_exit(1);
		}
	}

	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
}

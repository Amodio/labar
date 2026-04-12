#define _GNU_SOURCE

#include "exec.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

		char **tmp = realloc(argv, sizeof(char *) * (argc + 1));
		if (!tmp) {
			free(arg);
			for (int j = 0; j < argc; j++)
				free(argv[j]);
			free(argv);
			*argc_out = 0;
			return NULL;
		}
		argv = tmp;
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
				newv = realloc(newv, sizeof(char *) * (newc + 2));
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
				newv = realloc(newv, sizeof(char *) * (newc + 1));
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
			for (int j = 0; j < newc; j++)
				free(newv[j]);
			free(newv);
			return -1;
		}

		newv = realloc(newv, sizeof(char *) * (newc + 1));
		newv[newc++] = strdup(arg);
	}

	newv = realloc(newv, sizeof(char *) * (newc + 1));
	newv[newc] = NULL;

	// free original argv entries, they've all been strdup'd into newv
	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv); // free the old array itself too

	*argvp = newv;
	*argcp = newc;

	return 0;
}

static void
launch_app_second_fork(DesktopEntry *app, char **argv, int argc)
{
	errno = 0;
	pid_t ret = fork();
	if (ret == 0) {
		// Second child: execute the actual application
		if (app->terminal) {
			const char *term = getenv("TERM");

			if (!term) {
				term = DEFAULT_TERMINAL;
				if (verbose)
					printf(
						"[DBG] Cannot get the terminal, using the default one");
			}

			/*
			 * Build:
			 * terminal -e program arg1 arg2 ...
			 */
			char **term_argv = malloc(sizeof(char *) * (argc + 3));

			term_argv[0] = (char *)term;
			term_argv[1] = "-e";

			if (verbose >= 2)
				printf("[DBG²] -> launching app %s: %s %s", app->name,
					term_argv[0], term_argv[1]);
			for (int i = 0; i < argc; i++) {
				if (verbose >= 2)
					printf(" %s", argv[i]);
				term_argv[i + 2] = argv[i];
			}
			if (verbose >= 2)
				printf("\n");
			term_argv[argc + 2] = NULL;

			execvp(term, term_argv);
			fprintf(stderr, "ERROR: execvp terminal: %s\n", strerror(errno));
			_exit(EXIT_FAILURE);
		} else {
			if (verbose >= 2)
				printf("[DBG²] -> launching app %s: %s\n", app->name, argv[0]);

			execvp(argv[0], argv);
			fprintf(stderr, "ERROR: execvp: %s\n", strerror(errno));
			_exit(EXIT_FAILURE);
		}
	} else if (ret < 0) {
		fprintf(stderr, "ERROR: fork: %s\n", strerror(errno));
		_exit(EXIT_FAILURE);
	}
}

static void
launch_app_first_fork(DesktopEntry *app, char **argv, int argc)
{
	errno = 0;
	pid_t ret = fork();
	if (ret == 0) {
		// First child: create new session to detach from parent's process group
		setsid();

		// Restore signals to defaults (clear any inherited signal masks)
		sigset_t mask;
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, NULL);

		// Close and redirect standard file descriptors to /dev/null
		// to prevent holding onto the dock's terminal resources
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}

		// Fork again to create the actual child process
		launch_app_second_fork(app, argv, argc);
		_exit(EXIT_SUCCESS);
	} else if (ret < 0) {
		fprintf(stderr, "ERROR: fork: %s\n", strerror(errno));
	} else {
		// Parent: wait for first child to finish
		waitpid(ret, NULL, 0);
	}
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

	if (expand_field_codes(app, &argv, &argc) < 0) {
		for (int i = 0; i < argc; i++)
			free(argv[i]);
		free(argv);
		return;
	}

	launch_app_first_fork(app, argv, argc);

	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
}

// ---------------------------------------------------------------------------
// launch_command
//
// Launch a plain whitespace-separated command string (e.g. "foot -e btop").
// Uses a self-contained double-fork so labar doesn't collect zombie processes.
// stderr/stdout are intentionally kept open so the child can report errors.
// ---------------------------------------------------------------------------
void
launch_command(const char *cmd)
{
	if (!cmd || !cmd[0])
		return;

	if (verbose)
		printf("[DBG] -> launch_command: %s\n", cmd);

	int argc;
	char **argv = parse_exec(cmd, &argc);
	if (!argv || argc == 0)
		return;

	/* First fork: intermediate process */
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "ERROR: launch_command fork: %s\n", strerror(errno));
		goto cleanup;
	}
	if (pid == 0) {
		/* First child: detach from process group */
		setsid();

		/* Second fork: actual process (orphaned, adopted by init) */
		pid_t pid2 = fork();
		if (pid2 < 0) {
			_exit(EXIT_FAILURE);
		}
		if (pid2 == 0) {
			/* Second child: clean up inherited fds, reset signals,
			 * then exec the command. */

			/* Close all open file descriptors except stdin/stdout/stderr.
			 * This prevents foot from inheriting Wayland socket fds or
			 * other resources that could cause SIGHUP on close. */
			int maxfd = (int)sysconf(_SC_OPEN_MAX);
			if (maxfd < 0)
				maxfd = 1024;
			for (int fd = STDERR_FILENO + 1; fd < maxfd; fd++)
				close(fd);

			/* Redirect stdin/stdout/stderr to /dev/null */
			int devnull = open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > STDERR_FILENO)
					close(devnull);
			}

			/* Reset all signal handlers to defaults */
			struct sigaction sa = {.sa_handler = SIG_DFL};
			sigemptyset(&sa.sa_mask);
			for (int sig = 1; sig < NSIG; sig++)
				sigaction(sig, &sa, NULL);

			/* NULL-terminate argv for execvp */
			char **execargv = malloc(sizeof(char *) * (argc + 1));
			if (!execargv)
				_exit(EXIT_FAILURE);
			for (int i = 0; i < argc; i++)
				execargv[i] = argv[i];
			execargv[argc] = NULL;
			execvp(execargv[0], execargv);
			_exit(EXIT_FAILURE);
		}
		/* First child exits immediately so parent can waitpid cleanly */
		_exit(EXIT_SUCCESS);
	}

	/* Parent: reap first child immediately */
	waitpid(pid, NULL, 0);

cleanup:
	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
}

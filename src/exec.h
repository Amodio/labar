#ifndef EXEC_H
#define EXEC_H

#include "config.h"

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
static int expand_field_codes(DesktopEntry *app,
	char ***argvp,
	int *argcp);

/*
 * Launch application defined by DesktopEntry.
 * - Forks and execvp().
 * - Does not block.
 */
void launch_app(DesktopEntry *app);

#endif /* EXEC_H */

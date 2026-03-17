#ifndef EXEC_H
#define EXEC_H

#include "config.h"

#define DEFAULT_TERMINAL "foot"

/*
 * Launch application defined by DesktopEntry.
 * - Forks and execvp().
 * - Does not block.
 */
void launch_app(DesktopEntry *app);

/*
 * Launch a plain shell command string (e.g. "foot -e btop").
 * - Splits on whitespace, forks, and execvp().
 * - Does not block.
 */
void launch_command(const char *cmd);

#endif /* EXEC_H */

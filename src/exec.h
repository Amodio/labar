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

#endif /* EXEC_H */

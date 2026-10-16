/* Exercise the argv pointer budget separately from the command-string limit. */
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include "tests/lib.h"

static char command[4097];

static void
run_case (int arguments, int expected) {
  strlcpy (command, "  exec-limits", sizeof command);
  for (int i = 0; i < arguments; i++)
    strlcat (command, " x", sizeof command);
  pid_t pid = fork ("exec-child");
  if (pid == 0) {
    exec (command);
    fail ("exec returned");
  }
  if (pid < 0 || wait (pid) != expected)
    fail ("unexpected result for %d arguments", arguments);
}

int
main (int argc, char *argv[]) {
  test_name = "exec-limits";
  if (argc > 1) {
    if (argc != 201 || strcmp (argv[0], "exec-limits") || argv[argc] != NULL)
      fail ("invalid argument vector");
    for (int i = 1; i < argc; i++)
      if (strcmp (argv[i], "x"))
        fail ("invalid argument %d", i);
    return 42;
  }
  msg ("begin");
  run_case (200, 42);  /* Valid and larger than the old 100-element array. */
  run_case (450, -1);  /* Strings fit, but strings plus argv exceed one page. */
  run_case (520, -1);  /* Too many pointers even for the temporary argv page. */

  memset (command, 'x', sizeof command);
  command[sizeof command - 1] = '\0';
  pid_t pid = fork ("long-command");
  if (pid == 0) {
    exec (command);
    fail ("oversized exec returned");
  }
  if (pid < 0 || wait (pid) != -1)
    fail ("oversized command did not fail safely");
  msg ("end");
  return 0;
}

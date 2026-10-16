/* Force the parent to exit before its child, then reject a grandchild wait. */
#include <syscall.h>
#include "tests/lib.h"

static void
write_int (int fd, int value) {
  seek (fd, 0);
  if (write (fd, &value, sizeof value) != sizeof value)
    fail ("write failed");
}

static int
read_int (int fd) {
  int value = 0;
  seek (fd, 0);
  if (read (fd, &value, sizeof value) != sizeof value)
    fail ("read failed");
  return value;
}

/* Disk I/O sleeps while holding the global filesystem lock.  Give peers
 * a full user-mode time slice between polls so a poller cannot repeatedly
 * reacquire the lock before an equal-priority waiter runs. */
static void
poll_delay (void) {
  for (volatile unsigned i = 0; i < 10000000; i++) {}
}

int
main (void) {
  test_name = "orphan-wait";
  msg ("begin");
  if (!create ("gate", sizeof (int)) || !create ("done", sizeof (int)) ||
      !create ("grandchild", sizeof (int)))
    fail ("create failed");
  int gate = open ("gate");
  int done = open ("done");
  int child_id = open ("grandchild");
  if (gate < 2 || done < 2 || child_id < 2)
    fail ("open failed");

  pid_t parent = fork ("middle");
  if (parent == 0) {
    pid_t child = fork ("orphan");
    if (child < 0)
      fail ("orphan fork failed");
    if (child == 0) {
      while (read_int (gate) == 0)
        poll_delay ();
      write_int (done, 1);
      return 7;
    }
    write_int (child_id, child);
    return 12;
  }
  if (parent < 0 || wait (parent) != 12)
    fail ("parent did not exit");
  if (wait (parent) != -1 || wait (read_int (child_id)) != -1)
    fail ("accepted a repeated wait or a grandchild wait");
  write_int (gate, 1);
  while (read_int (done) == 0)
    poll_delay ();
  close (gate);
  close (done);
  close (child_id);
  msg ("end");
  return 0;
}

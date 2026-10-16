/* A mapped pointer is insufficient: permissions and the full range matter. */
#include <limits.h>
#include <stdint.h>
#include <syscall.h>
#include "tests/lib.h"

static const char readonly[] = "abc";
static char buffer[16];

int
main (void) {
  test_name = "memory-access";
  msg ("begin");
  int fd = open ("sample.txt");
  if (fd < 2)
    fail ("open failed");
  for (int mode = 0; mode < 4; mode++) {
    pid_t pid = fork ("bad-memory");
    if (pid == 0) {
      switch (mode) {
        case 0:
          read (fd, (void *) readonly, 1);
          break;
        case 1:
          write (fd, buffer, UINT_MAX);
          break;
        case 2:
          write (fd, (void *) (uintptr_t) -8, 16);
          break;
        case 3:
          /* Starts in mapped data; the rest is far outside this program. */
          read (fd, buffer, 0x10000000);
          break;
      }
      fail ("invalid memory was accepted");
    }
    if (pid < 0 || wait (pid) != -1)
      fail ("bad-memory case %d did not exit with -1", mode);
  }
  if (write (fd, readonly, sizeof readonly) != sizeof readonly)
    fail ("writing from read-only memory failed");
  if (read (fd, NULL, 0) != 0 || write (fd, NULL, 0) != 0)
    fail ("zero-length I/O failed");
  if (create ("huge", UINT_MAX))
    fail ("unrepresentable file size was accepted");
  close (fd);
  msg ("end");
  return 0;
}

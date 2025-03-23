#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char* argv[])
{
  int fd1[2],fd2[2];
  pipe(fd1);
  pipe(fd2);

  // create a child process
  // child process: first read, then write
  if (fork() == 0) {
    char buf[10];
    close(fd1[1]);
    close(fd2[0]);
    while (read(fd1[0], buf, 1) > 0) {
        printf("%d: received ping\n", getpid());
        close(fd1[0]);
        write(fd2[1], "ping", 1);
        close(fd2[1]);
    }
  }
  // parent process: first write, then read
  else {
    close(fd1[0]);
    close(fd2[1]);
    write(fd1[1], "ping", 1);
    close(fd1[1]);
    char buf[10];
    while (read(fd2[0], buf, 1) > 0) {
        printf("%d: received pong\n", getpid());
        close(fd2[0]);
    }
  }

  exit(0);
}

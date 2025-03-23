#include "kernel/types.h"
#include "user/user.h"
#include "kernel/sysinfo.h"

int
main(int argc, char *argv[])
{
  if(argc != 1){
    fprintf(2, "Usage: sysinfo\nPrint the system information.\n");
    exit(1);
  }

  struct sysinfo info;
  sysinfo(&info);

  printf("Free memory: %d bytes\n", info.freemem);
  printf("Number of processes: %d\n", info.nproc);
  exit(0);
}

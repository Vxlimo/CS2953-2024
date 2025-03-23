#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char* argv[])
{
  if (argc != 2) {
    fprintf(2, "Usage: sleep NUMBER\nPause for NUMBER ticks, NUMBER\n");
    exit(1);
  }

  // check if the argument is a number
  for (int i = 0; i < strlen(argv[1]); i++) {
    if (argv[1][i] < '0' || argv[1][i] > '9') {
        fprintf(2, "Invalid NUMBER.\nUsage: sleep NUMBER\nPause for NUMBER ticks.\n");
        exit(1);
    }
  }

  int time = atoi(argv[1]);
  sleep(time);

  exit(0);
}

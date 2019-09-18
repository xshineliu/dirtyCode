// based on https://github.com/lxc/lxcfs/issues/125

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#define STAT_FILE "/proc/stat"

#define MAX_SIZE (4096 * 1024)
static char buf3[MAX_SIZE];

int getbtime(int bufsz) {
    static unsigned long btime = 0;
    bool found_btime = false;
    FILE *f;
    int fd = -1;

    if (btime)
        return btime;

    /* /proc/stat can get very large on multi-CPU systems so we
       can't use FILE_TO_BUF */
    if ((fd = open(STAT_FILE, 0)) < 0) {
        fputs("can not open"STAT_FILE, stderr);
        fflush(NULL);
        exit(102);
    }
    /* gets seems to give 0 here in some cases inside containers
       before EOF is reached */
    int sz = 0;
    int tsz = 0;
    int i = 0;
    while ((sz = read(fd, buf3, bufsz)) > 0) {
    	fprintf(stderr, "loop %d: Got %d\n", i++, sz);
    	tsz += sz;
    }
    close(fd);

    return tsz;
}

int main(int argc, char* argv[]) {
  if(argc < 2) {
     fprintf(stderr, "Need buf size provided\n");
     exit(0);
  }
  int bufsz = atol(argv[1]);
  if(bufsz < 0) {
     bufsz = 1;
  }
  printf("With bufsize to %d, total size got %d\n", bufsz, getbtime(bufsz));
  exit(0);
}

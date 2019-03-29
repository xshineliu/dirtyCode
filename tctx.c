// Copyright (C) 2010  Benoit Sigoure
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// gcc -o tctx tctx.c -std=c99 -lpthread
//#define _XOPEN_SOURCE
#define _GNU_SOURCE

#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/futex.h>


// REF1: https://eli.thegreenplace.net/2018/basics-of-futexes/


static inline long long unsigned time_ns(struct timespec* const ts) {
  if (clock_gettime(CLOCK_REALTIME, ts)) {
    exit(1);
  }
  return ((long long unsigned) ts->tv_sec) * 1000000000LLU
    + (long long unsigned) ts->tv_nsec;
}

int iterations = 500000;
int debug = 0;

void* thread(void* ftx) {
  unsigned long long cnt1 = 0;
  unsigned long long cnt2 = 0;
  unsigned long long cnt3 = 0;
  //unsigned long long cnt4 = 0;
  unsigned long long missed1 = 0;
  unsigned long long retcnt = 0;
  int ret = 0;
  int* futex = (int*) ftx;
  for (int i = 0; i < iterations; i++) {
    //sched_yield();
    cnt1++;
    while ((ret = syscall(SYS_futex, futex, FUTEX_WAIT, 0xA, NULL, NULL, 42)) != 0) {
      // retry
      //sched_yield();
      cnt2++;
      if(ret == -1) {
        if(errno != EAGAIN) {
          exit(1);
        } else {
          missed1++;
          continue;
        }
      }
    }
    // ret == 0, be waken

    *futex = 0xB;
    while ((ret = syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 42)) == 0) {
      // retry
      //sched_yield();
      cnt3++;
    }
    retcnt += ret;
  }

  fprintf(stdout, "CHLD: %lld wait=%lld wake=%lld AGAIN=%lld %lld\n", cnt1, cnt2, cnt3, missed1, retcnt);
  return NULL;
}

int main(int argc, char *argv[]) {


  if(argc < 2) {
    fprintf(stderr, "Args needed\n");
    exit (1);
  }

  iterations = atoi(argv[1]);

  struct timespec ts;
  const int shm_id = shmget(IPC_PRIVATE, sizeof (int), IPC_CREAT | 0666);
  int* futex = shmat(shm_id, NULL, 0);
  *futex = 0xA;

  pthread_t thd;
  if (pthread_create(&thd, NULL, thread, futex)) {
    return 1;
  }

  int ret = 0;

  unsigned long long cnt1 = 0;
  unsigned long long cnt2 = 0;
  unsigned long long cnt3 = 0;
  //unsigned long long cnt4 = 0;
  unsigned long long missed1 = 0;
  unsigned long long retcnt = 0;

  const long long unsigned start_ns = time_ns(&ts);
  for (int i = 0; i < iterations; i++) {
    *futex = 0xA;
    cnt1++;
    while ((ret = syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 42)) == 0) {
      // retry
      // sched_yield();
      cnt3++;
    }
    retcnt += ret;

    //sched_yield();
    while ((ret = syscall(SYS_futex, futex, FUTEX_WAIT, 0xB, NULL, NULL, 42) != 0)) {
      // retry
      cnt2++;
      if(ret == -1) {
        if(errno != EAGAIN) {
          exit(1);
        } else {
          missed1++;
          continue;
        }
      }
      //sched_yield();
    }
  }
  const long long unsigned delta = time_ns(&ts) - start_ns;

  fprintf(stdout, "MAIN: %lld wait=%lld wake=%lld AGAIN=%lld %lld\n", cnt1, cnt2, cnt3, missed1, retcnt);

  //const int nswitches = iterations << 2;
  printf("%i  thread context switches in %llu ns (%.1f ns/ctxsw)\n",
         cnt1, delta, (delta / (double) cnt1));
  //wait(futex);
  pthread_join(thd, (void **)&futex);
  return 0;
}

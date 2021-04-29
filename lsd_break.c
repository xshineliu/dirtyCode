/*
 * find the LSD break point, resuse the latency microbenchmark
 * taskset -c 0-13 ./turboTesting -d 10 -n 14 | tee - | grep "^Thread" | awk -F\= '{++S[$NF]; d+=$NF} END {printf "%.02f\n", d/NR/1000000000.0}'
 * taskset -c 0 ./turboTesting -d 10 -n 1 | tee - | grep "^Thread" | awk -F\= '{++S[$NF]; d+=$NF} END {printf "%.02f\n", d/NR/1000000000.0}'
 * for i in `seq 0 1 13`; do taskset -c 0-$i ./b -d 10 -n $((i + 1)) | tee - | grep "^Thread" | awk -F\= '{++S[$NF]; d+=$NF} END {printf "%.02f\n", d/NR/1000000000.0}'; done
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_THREADS (1024)
#define LOOP (10 * 1000 * 1000)

#ifndef L1D_CL_BIT_SHIFT
#define L1D_CL_BIT_SHIFT (7)
#endif

#define L1D_CL_SIZE (1 << L1D_CL_BIT_SHIFT)


char data[MAX_THREADS + 1][L1D_CL_SIZE] __attribute__((aligned(L1D_CL_SIZE)));
int num_threads = 2;
int cpubind = 0;
volatile int timeout = 0; /* this data is shared by the thread(s) */
int delay_sec = 10;
unsigned long long count1 = 3100000;
unsigned long long count2 = 1000;

static inline long long unsigned time_ns(struct timespec* const ts) {
        if (clock_gettime(CLOCK_REALTIME, ts)) {
                exit(1);
        }
        return ((long long unsigned) ts->tv_sec) * 1000000000LLU
                + (long long unsigned) ts->tv_nsec;
}

static void print_help(const char *progname)
{
        printf("Usage: %s [OPTS]\n", progname);
        printf("-d      delay in second\n");
        printf("-n      number of threads\n");
        printf("-c      test case\n");
        printf("-b      Silly bind to each core\n");
        printf("-v      debug level\n");
}


void taskbind(int i) {
        if(!cpubind) {
                return;
        }
        cpu_set_t my_set;        /* Define your cpu_set bit mask. */
        CPU_ZERO(&my_set);       /* Initialize it all to 0, i.e. no CPUs selected. */
        CPU_SET(i, &my_set);     /* set the bit that represents core 7. */
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set); /* Set affinity of tihs process to */
}


//#pragma GCC push_options
//#pragma GCC optimize ("align-functions=4096")

// data must point to a valid address
unsigned long long __attribute__ ((aligned (64), noinline )) delay_with_cnt(unsigned long long cnt1, unsigned long long cnt2, void* dst, void* src, size_t len, void* call) {
  __asm__ __volatile__
    (
     "  nop\n"  // 001
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"  // 010
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"  // 020
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"  // 030
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"  // 040
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"
     "  nop\n"  // 050
     "  nop\n"  // 051
     "  nop\n"  // 052
     "  nop\n"  // 053
     //"  nop\n"  // 054
     //"  nop\n"  // 055

// 10 bytes push 2, push 2, mov 3, mov 3

     "  push   %r11\n"
     "  push   %r10\n"
     "  mov    %rsi, %r11\n"
     "  1:\n"
     "  mov    %rdi, %r10\n"

// dec 3, cmp 4, ja 2

     "  2:\n"
     "  dec    %r10\n"
     "  cmp    $0x0, %r10\n"
     "  ja     2b\n"
/*
     "  push   %rsi\n"
     "  push   %r11\n"
     "  push   %rdi\n"
     "  push   %r10\n"
     "  push   %rcx\n"
     "  push   %rdx\n"
     "  push   %r8\n"
     "  push   %r9\n"
     "  mov    %rdx, %rdi\n"
     "  mov    %rcx, %rsi\n"
     "  mov    %r8, %rdx\n"
     "  call   *%r9\n"
     "  pop    %r9\n"
     "  pop    %r8\n"
     "  pop    %rdx\n"
     "  pop    %rcx\n"
     "  pop    %r10\n"
     "  pop    %rdi\n"
     "  pop    %r11\n"
     "  pop    %rsi\n"
*/
     "  dec    %r11\n"
     "  cmp    $0x0, %r11\n"
     "  ja     1b\n"

     "  pop    %r10\n"
     "  pop    %r11\n"
     "  mov    %rcx, %rax\n"
     );
}

//#pragma GCC pop_options


void *counting(void * param)
{
        int i, id;
        pid_t tid;
        struct timespec ts;
        unsigned long long start_ns;
        unsigned long long delta1 = 0;
        unsigned long long *p = (unsigned long long *) param;
        long offset = (char *)param - (char *)data;
        id = (int) (offset >> L1D_CL_BIT_SHIFT);
        taskbind(id);
        start_ns = time_ns(&ts);

        //delay_with_cnt(3100000, 1000, NULL, NULL, 0, NULL);
        delay_with_cnt(count1, count2, NULL, NULL, 0, NULL);

        delta1 = time_ns(&ts) - start_ns;
        tid = syscall(SYS_gettid);
	    printf("Thread id=%04d, tid=%06d, addr=%p, cost=%llu\n", id, tid, p, delta1);
        pthread_exit(0);
}




int main(int argc, char *argv[])
{
        int i, j, opt;
        pthread_t tid[MAX_THREADS];              /* the thread identifiers      */
        pthread_attr_t attr; /* set of thread attributes */
        char *p = NULL;

        while ((opt = getopt(argc, argv, "n:d:x:y:b")) != -1) {
                switch (opt) {
                case 'n':
                        num_threads = strtoul(optarg, NULL, 0);
                        break;
                case 'd':
                        delay_sec = strtoul(optarg, NULL, 0);
                        break;
                case 'x':
                        count1 = strtoull(optarg, NULL, 0);
                        break;
                case 'y':
                        count2 = strtoull(optarg, NULL, 0);
                        break;
                case 'b':
                        cpubind = 1;
                        break;
                default:
                        print_help(argv[0]);
                        return 0;
                }
        }

        if (num_threads < 1) {
                fprintf(stderr,"%d must be >= 1\n", num_threads);
                exit(EXIT_FAILURE);
        }

        if (num_threads > MAX_THREADS) {
                fprintf(stderr,"%d must be <= %d\n", num_threads, MAX_THREADS);
                exit(EXIT_FAILURE);
        }

        if (delay_sec < 1) {
                fprintf(stderr,"%d must be >= 1\n", delay_sec);
                exit(EXIT_FAILURE);
        }

        printf("The number of threads is %d, delay %d.\n", num_threads, delay_sec);

        /* get the default attributes */
        pthread_attr_init(&attr);

        p = (char *)data;
        /* create the threads */
        for (i = 0; i < num_threads; i++) {
                *(long long *)p = 0L;
                pthread_create(&(tid[i]), &attr, counting, (void *)p);
                p += L1D_CL_SIZE;
        }

        printf("Created %d threads with test case %d.\n", num_threads, count1);


        //sleep(delay_sec);
        timeout = 1;

        /* now wait for the threads to exit */
        for (i = 0; i < num_threads; i++) {
                pthread_join(tid[i], NULL);
        }


        printf("----------\n");
        return 0;
}

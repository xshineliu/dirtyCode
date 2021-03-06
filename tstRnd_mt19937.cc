// g++ -o tstRnd -O0 ./tstRnd.cc -std=c++11 -fopenmp
// Gold 5118: 12 Cores x 10 M Same SKT = 24.3; 12 Cores x 5 M Same SKT with HT  = 26.4;
// 24 Cores x 5 M Dual SKT = 27.8; 12 Cores x 10 M Dual SKT = 28.2; 
// E5-2650 v4: 21.3 / 25.6 / 27.5 / 23.8
// ref1: https://diego.assencio.com/?index=6890b8c50169ef45b74db135063c227c
// ref2: https://www.guyrutenberg.com/2014/05/03/c-mt19937-example/

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <omp.h>
#include <unistd.h>
#include <iostream>
#include <random>
 
using namespace std;

#define MAX_CORES               4096
#define DEFAULT_LOOP    (100*1000*1000)

long long unsigned start_ns;
struct timespec ts;
int cnt[MAX_CORES];

volatile int token  __attribute__((aligned(0x1000)));

static inline long long unsigned time_ns(struct timespec* const ts) {
        if (clock_gettime(CLOCK_REALTIME, ts)) {
                exit(1);
        }
        return ((long long unsigned) ts->tv_sec) * 1000000000LLU
                        + (long long unsigned) ts->tv_nsec;
}


int measure(int loops) {
        long long unsigned delta;
        int idx = 0;
        int rand = 0;
        int i = 0;
        int n_threads = 1; // omp_get_num_procs();  & omp_get_num_threads() doesn't work;

        token = 0;

#pragma omp parallel
        {
                n_threads = omp_get_num_threads();
                printf("#### %04d/%04d\n", omp_get_thread_num(), n_threads);

        }

          start_ns = time_ns(&ts);

#pragma omp parallel private(idx, rand, i)
        {
                idx = omp_get_thread_num();
	            	mt19937 mt_rand(time(0) | idx );
                for(i = 0; i < loops; i++) {
                        // busy wait until the token has been passed, Invalid in L1 & L2
                        rand += mt_rand();
                }

        }

          delta = time_ns(&ts) - start_ns;
          printf("RES: TIME_ALL %.06f PER_LOOP %.03f BW %.03f\n", (double)delta / 1000000.0f, (double)delta / (double)loops, (double)n_threads * (double)loops * 1000.0f / (double)delta);
	return rand;
}

int main(int argc, char *argv[]) {

        int loops = 0;
        int ret = 0;
        if(argc > 1) {
                loops = atoi(argv[1]);
        }
        if(loops < 10) {
                loops = DEFAULT_LOOP;
        }
        printf("Use loops = %d\n", loops);
        ret = measure(loops);

        return EXIT_SUCCESS;
}

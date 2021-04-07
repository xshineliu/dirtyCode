/*
 * gcc -O2 -o testFunc -fno-plt -static  testFunc.c
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <unistd.h>
#include <sys/mman.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/time.h>
#include <sys/resource.h>

void *(*my_memcpy)(void *dest, const void *src, size_t n);

/* Linux Only */
#define DEF_HUGE_PAGE_SIZE (2 * 1024 * 1024)

#define MAX_GAP (2 * 1024 * 1024)
#define DEF_PAGE_SIZE (4 * 1024)

#define CP_ONE {(*my_memcpy)(p2, p1, block_size);}
#define CP_FIVE CP_ONE CP_ONE CP_ONE CP_ONE CP_ONE
#define CP_TEN  CP_FIVE CP_FIVE
#define CP_FIFTY        CP_TEN CP_TEN CP_TEN CP_TEN CP_TEN
#define CP_HUNDRED      CP_FIFTY CP_FIFTY


// global    ///////////
float ms_pf = 0.0f;
float ms_rndset = 0.0f;
float rnd_mul = 0.0f;
float avg_ld_cost = 0.0f;
float use_ratio = 1.0f;
// global end ///////////

static inline unsigned long long time_us_timeval(struct timeval* const tv) {
	return ((unsigned long long) tv->tv_sec) * 1000000LLU
			+ (unsigned long long) tv->tv_usec;
}

static inline unsigned long long time_ns(struct timespec* const ts) {
	if (clock_gettime(CLOCK_REALTIME, ts)) {
		exit(1);
	}
	return ((unsigned long long) ts->tv_sec) * 1000000000LLU
			+ (unsigned long long) ts->tv_nsec;
}

void* dummyMemcpy(void* dest, const void* src, size_t sz) {
/*
  __memcpy_avx_unaligned_erms():
  mov        %rdi,%rax
  cmp        $0x20,%rdx
  jb         52
  cmp        $0x40,%rdx
  ja         b2
  vmovdqu    (%rsi),%ymm0
  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1
  vmovdqu    %ymm0,(%rdi)
  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)
  vzeroupper
  retq
*/

  __asm__ __volatile__
    (
     //"  .global __asm_dummy_memcpy\n"
     //"__asm_dummy_memcpy:\n"
     "  mov    %rdi, %rax\n"
     "  cmp    $0x20, %rdx\n"
     "  jb     1f\n"
     "  cmp    $0x40, %rdx\n"
     "  ja     1f\n"
     " 1:\n"
     //"  ret\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* my_memcpy_64(void* dest, const void* src, size_t sz) {
/*
  __memcpy_avx_unaligned_erms():
  mov        %rdi,%rax
  cmp        $0x20,%rdx
  jb         52
  cmp        $0x40,%rdx
  ja         b2
  vmovdqu    (%rsi),%ymm0
  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1
  vmovdqu    %ymm0,(%rdi)
  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)
  vzeroupper
  retq
*/

  __asm__ __volatile__
    (
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     );
}

// caller must make sure the address alligned to 64 bytes
void* my_memcpy_emovsb(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  mov        %rdx,%rcx\n"
     "  rep        movsb %ds:(%rsi),%es:(%rdi)\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* my_memcpy_64_unroll_10(void* dest, const void* src, size_t sz) {
/*
  __memcpy_avx_unaligned_erms():
  mov        %rdi,%rax
  cmp        $0x20,%rdx
  jb         52
  cmp        $0x40,%rdx
  ja         b2
  vmovdqu    (%rsi),%ymm0
  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1
  vmovdqu    %ymm0,(%rdi)
  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)
  vzeroupper
  retq
*/

  __asm__ __volatile__
    (
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"

     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    -0x20(%rsi,%rdx,1),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,-0x20(%rdi,%rdx,1)\n"
     //"  vzeroupper\n"


     );
}




int hugepage_forced = 0;
void* alloc_memory(size_t n_bytes) {
	void *ptr = NULL;
	if(hugepage_forced) {
		ptr = mmap(NULL, ((n_bytes < DEF_HUGE_PAGE_SIZE) ? DEF_HUGE_PAGE_SIZE : n_bytes),
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0) ;
		if(ptr != MAP_FAILED) {
			//printf("HugePage allocated at pointer %p\n", ptr);
			return ptr;
		} else {
            //printf("HugePage allocated Failed: %p\n", ptr);
		}
	}
	// fall back to normal page allocation
	int ret = 0;
	ret = posix_memalign((void **)&ptr, sysconf(_SC_PAGESIZE), n_bytes);
	if(ret) {
		//fprintf(stderr,"Normal page allocation failure due to None zero ret code %d\n", ret);
		exit(EXIT_FAILURE);
	} else {
		//printf("Normal page allocated at pointer %p\n", ptr);
	}
	return ptr;
}


void core_test(size_t block_size, size_t off1, size_t off2, size_t gap, unsigned long long int itr1) {
	unsigned long long delta1 = 0, delta2 = 0;
	unsigned long long i = 0;
	register char *m1 = NULL, *m2 = NULL;
	register char *p1 = NULL, *p2 = NULL;
	unsigned long long start_ns;
	struct timespec ts;
	struct rusage r1, r2;
	size_t to_alloc = 0;
	unsigned long long int itr2 = 100;

	if(block_size < DEF_PAGE_SIZE) {
		to_alloc = DEF_PAGE_SIZE;
	} else {
		to_alloc = block_size;
	}

    // add guard page in the end
    if(gap != 0) {
	m1 = alloc_memory(to_alloc + 3 * MAX_GAP);
        memset(m1, 0, to_alloc + 3 * MAX_GAP);
        p1 = m1 + off1;
	p2 = p1 + gap;
    } else {
	m1 = alloc_memory(to_alloc + MAX_GAP);
        memset(m1, 0, to_alloc + MAX_GAP);
	m2 = alloc_memory(to_alloc + MAX_GAP);
        memset(m2, 0, to_alloc + MAX_GAP);
        p1 = m1 + off1;
        p2 = m2 + off2;
    }

	printf("%d %p %p 0x%llx\t", block_size, p1, p2, (p1 > p2) ? (p1 - p2) : (p2 - p1));

	memset(&r1, 0, sizeof(r1));
	memset(&r1, 0, sizeof(r2));

	getrusage(RUSAGE_THREAD, &r1);
	start_ns = time_ns(&ts);
	unsigned long long loops = itr1 * itr2;
	while (itr1-- > 0) {
		for (i = 0; i < itr2; i++) {
			//CP_ONE
			//CP_TEN
			CP_HUNDRED
			;
		}
	}

	delta1 = time_ns(&ts) - start_ns;
	getrusage(RUSAGE_THREAD, &r2);
	delta2 = (time_us_timeval(&(r2.ru_utime)) - time_us_timeval(&(r1.ru_utime))) * 1000UL;
	/* avoid compiler optimization */

	printf("%llu %llu %.2f %.2f\n", delta1, delta2,
		       	(double)(delta1) / (double)(loops),
			(double)(delta2) / (double)(loops));
}



int main(int argc, char *argv[]) {
	int opt = 0, testCase = 0;

	unsigned long long *pos = NULL, *ptr = NULL, n_bytes = 0;
	unsigned long long start_ns;
	struct timespec ts;

	unsigned long mem_size_per_thread_in_kb = 4;
	unsigned int scale = 1;
	size_t seg_size = 64;
	size_t seg_gap = 0;
	size_t off1 = 0;
	size_t off2 = 0;
	double delta = 0.0f;
	unsigned long long repeat = 1000UL * 10UL;
	// default case
	my_memcpy = memcpy;

	while ((opt = getopt(argc, argv, "s:mx:y:lg:r:c:")) != -1) {
		switch (opt) {
		case 's':
			seg_size = strtoul(optarg, NULL, 0);
			break;
		case 'm':
			scale = 1024L;
			break;
		case 'l':
			hugepage_forced = 1;
			break;
		case 'c':
			testCase = strtoul(optarg, NULL, 0);
			break;
		case 'x':
			off1 = strtoul(optarg, NULL, 0);
			break;
		case 'y':
			off2 = strtoul(optarg, NULL, 0);
			break;
		case 'g':
			seg_gap = strtoul(optarg, NULL, 0);
			break;
		case 'r':
			repeat = strtoul(optarg, NULL, 0);
			break;
		default:
			exit(EXIT_FAILURE);
		}
	}

	switch(testCase) {
		case 0:
			my_memcpy = memcpy;
			break;
		case 1:
			my_memcpy = dummyMemcpy;
			break;
		case 2:
			my_memcpy = my_memcpy_64;
			break;
		case 3:
			my_memcpy = my_memcpy_64_unroll_10;
			break;
		case 4:
			my_memcpy = my_memcpy_emovsb;
			break;
	}
/*
	mem_size_per_thread_in_kb *= scale;
	if (mem_size_per_thread_in_kb < 4) {
		fprintf(stderr, "%d must be >= 4\n", mem_size_per_thread_in_kb);
		exit(EXIT_FAILURE);
	}
*/

    if (off1 + off2 + seg_gap > 3 * MAX_GAP) {
		fprintf(stderr, "total gap must be <= 0x%llx\n", 3 * MAX_GAP);
    }

	core_test(seg_size, off1, off2, seg_gap, repeat);
	return EXIT_SUCCESS;
}

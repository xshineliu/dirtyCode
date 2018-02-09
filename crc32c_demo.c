#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
//#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#include <x86intrin.h>


#define N 4096
#define MIN_N_ELEM 16
#define VAL_TYPE unsigned long long
#define TYPE_LEN (sizeof(VAL_TYPE))

#ifndef DEF_HUGE_PAGE_SIZE
#define DEF_HUGE_PAGE_SIZE ((0x200000))
#endif

void *ptr = NULL;


void crc_burn(void *p, size_t len)
{
	asm(
		"mov   $0x0,%r9\n\t"
		"mov   %rdi,%rsi\n\t"
		"mov   %rdi,%r8\n\t"
		"push %r10\n\t"
		"push %r11\n\t"
		"push %r12\n\t"
		"LBL_BGN:\n\t"
		"crc32q 0x0(%rdi),%r10\n\t"
		"crc32q 0x8(%rsi),%r11\n\t"
		"crc32q 0x10(%r8),%r12\n\t"
		"crc32q 0x18(%rdi),%r10\n\t"
		"crc32q 0x20(%rsi),%r11\n\t"
		"crc32q 0x28(%r8),%r12\n\t"
		"crc32q 0x30(%rdi),%r10\n\t"
		"crc32q 0x38(%rsi),%r11\n\t"
		"crc32q 0x40(%r8),%r12\n\t"
		"crc32q 0x48(%rdi),%r10\n\t"
		"crc32q 0x50(%rsi),%r11\n\t"
		"crc32q 0x58(%r8),%r12\n\t"
		"crc32q 0x60(%rdi),%r10\n\t"
		"crc32q 0x68(%rsi),%r11\n\t"
		"crc32q 0x70(%r8),%r12\n\t"
		"crc32q 0x78(%rdi),%r10\n\t"
		"crc32q 0x80(%rsi),%r11\n\t"
		"crc32q 0x88(%r8),%r12\n\t"
		"crc32q 0x90(%rdi),%r10\n\t"
		"crc32q 0x98(%rsi),%r11\n\t"
		"crc32q 0xa0(%r8),%r12\n\t"
		"crc32q 0xa8(%rdi),%r10\n\t"
		"crc32q 0xb0(%rsi),%r11\n\t"
		"crc32q 0xb8(%r8),%r12\n\t"
		"crc32q 0xc0(%rdi),%r10\n\t"
		"crc32q 0xc8(%rsi),%r11\n\t"
		"crc32q 0xd0(%r8),%r12\n\t"
		"crc32q 0xd8(%rdi),%r10\n\t"
		"crc32q 0xe0(%rsi),%r11\n\t"
		"crc32q 0xe8(%r8),%r12\n\t"
		"crc32q 0xf0(%rdi),%r10\n\t"
		"crc32q 0xf8(%rsi),%r11\n\t"
		"add $0x1, %r9\n\t"
		"cmp $0x1DCD64FF, %r9\n\t"
		"jle LBL_BGN\n\t"
		"pop %r12\n\t"
		"pop %r11\n\t"
		"pop %r10"
		);
}




long long unsigned start_ns;
struct timespec ts;


static inline long long unsigned time_ns(struct timespec* const ts) {
        if (clock_gettime(CLOCK_REALTIME, ts)) {
                exit(1);
        }
        return ((long long unsigned) ts->tv_sec) * 1000000000LLU
                        + (long long unsigned) ts->tv_nsec;
}



inline void allocMem(void **pptr, unsigned long long n_bytes, int hugepage_forced) {

        int hugepage_allocate = 0;
	int ret;
        if(hugepage_forced) {
                *pptr = mmap(NULL, ((n_bytes < DEF_HUGE_PAGE_SIZE) ? DEF_HUGE_PAGE_SIZE : n_bytes),
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0) ;
                if(*pptr != MAP_FAILED) {
                        printf("HugePage allocated at pointer %p with size %llX\n", *pptr, n_bytes);
                        hugepage_allocate = 1;
                } else {
                        printf("HugePage allocated Failed: %d\n", *pptr);
                }
        }


        if(!hugepage_allocate) {
                ret = posix_memalign(pptr, sysconf(_SC_PAGESIZE), n_bytes);
                if (ret) {
                        fprintf(stderr,"None zero ret code %d\n", ret);
                        exit(EXIT_FAILURE);
                }
                printf("Normal page allocated at pointer %p with size %llX,\n\t" \
                        " * Please manually disable the Transparent Huge Page if needed.\n", *pptr, n_bytes);
	}
}


int main(int argc, char* argv[])
{
	unsigned long long *pos;
	unsigned long long i;
	unsigned long long start_ns;
	unsigned long long delta;
	struct timespec ts;

	int ret = 0;
	int hugepage_forced = 1;
	unsigned long long n_bytes = TYPE_LEN * N;
	unsigned long long steps = n_bytes / sizeof(unsigned long long);


	allocMem(&ptr, n_bytes, hugepage_forced);
	pos = (unsigned long long *)ptr;


	start_ns = time_ns(&ts);

	crc_burn((void *)ptr, n_bytes);

    delta = time_ns(&ts) - start_ns;
	printf("%p %016llX %ld %ld\n", pos, *(pos + 1), delta, steps);

}

/*
 * gcc -O2 -o testFunc.static -fno-plt -static  testFunc.c
 * gcc -O2 -o testFunc testFunc.c
 */

// force each function strict align to cache line size
#pragma GCC optimize ("align-functions=64")

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


void vporymm_vz();
void vporzmm_vz();

// data must point to a valid address
unsigned long long __attribute__ ((noinline)) my_lat_measure(unsigned long long cnt1, unsigned long long cnt2, void* dst, void* src) {
  __asm__ __volatile__
    (
     "  mov    %rsi, %r11\n"
     "  1:\n"
     "  mov    %rdi, %r10\n"

     "  2:\n"
     "  dec    %r10\n"
     "  cmp    $0x0, %r10\n"
     "  ja     2b\n"

     "  vmovdqu    (%rcx),%ymm0\n"
     "  vmovdqu    0x20(%rcx),%ymm1\n"
     "  vmovdqu    %ymm0,(%rdx)\n"
     "  vmovdqu    %ymm1,0x20(%rdx)\n"

     //"  vmovdqu64    (%rcx),%zmm0\n"
     //"  vmovdqu64    %zmm0,(%rdx)\n"

     "  dec    %r11\n"
     "  cmp    $0x0, %r11\n"
     "  ja     1b\n"

     "  mov    %rcx, %rax\n"
     );
}

// data must point to a valid address
unsigned long long __attribute__ ((noinline)) my_delay(unsigned long long cnt1, unsigned long long cnt2) {
  __asm__ __volatile__
    (
     "  mov    %rsi, %r11\n"
     "  1:\n"
     "  mov    %rdi, %rcx\n"

     "  2:\n"
     "  dec    %rcx\n"
     "  cmp    $0x0, %rcx\n"
     "  ja     2b\n"
     "  vpord  %zmm0, %zmm0, %zmm0\n"
     "  dec    %r11\n"
     "  cmp    $0x0, %r11\n"
     "  ja     1b\n"

     "  mov    %rcx, %rax\n"
     );
}


// data must point to a valid address
void my_lat_measure_wrapper(unsigned long long cnt1, unsigned long long cnt2){
	unsigned long long delta1 = 0, delta2 = 0;
	unsigned long long i = 0;
	unsigned long long start_ns;
	struct timespec ts;
	struct rusage r1, r2;

	void *p1 = alloc_memory(MAX_GAP);
	memset(p1, 0, MAX_GAP);
	void *dst = p1;
	void *src = p1 + DEF_PAGE_SIZE;

	memset(&r1, 0, sizeof(r1));
	memset(&r1, 0, sizeof(r2));

	getrusage(RUSAGE_THREAD, &r1);
	start_ns = time_ns(&ts);
	unsigned long long loops = cnt1 * cnt2;

	my_lat_measure(cnt1, cnt2, dst, src);

	delta1 = time_ns(&ts) - start_ns;
	getrusage(RUSAGE_THREAD, &r2);
	delta2 = (time_us_timeval(&(r2.ru_utime)) - time_us_timeval(&(r1.ru_utime))) * 1000UL;

	printf("%llu %llu %.3f %.3f\n", delta1, delta2,
		       	(double) loops / (double)delta1, (double) loops / (double)delta2);
}

// data must point to a valid address
void my_delay_wrapper(unsigned long long cnt1, unsigned long long cnt2) {
	unsigned long long delta1 = 0, delta2 = 0;
	unsigned long long i = 0;
	unsigned long long start_ns;
	struct timespec ts;
	struct rusage r1, r2;

	memset(&r1, 0, sizeof(r1));
	memset(&r1, 0, sizeof(r2));

	getrusage(RUSAGE_THREAD, &r1);
	start_ns = time_ns(&ts);
	unsigned long long loops = cnt1 * cnt2;

	my_delay(cnt1, cnt2);

	delta1 = time_ns(&ts) - start_ns;
	getrusage(RUSAGE_THREAD, &r2);
	delta2 = (time_us_timeval(&(r2.ru_utime)) - time_us_timeval(&(r1.ru_utime))) * 1000UL;

	printf("%llu %llu %.3f %.3f\n", delta1, delta2,
		       	(double) loops / (double)delta1, (double) loops / (double)delta2);
}


void __attribute__ ((noinline)) miscFunc(void) {
  __asm__ __volatile__
    (
     //"  .global vporymm_vz\n"
     "  vporymm_vz:\n"
     "  vpor   %ymm0, %ymm0, %ymm0\n"
     "  vpor   %ymm0, %ymm0, %ymm0\n"
     "  vpor   %ymm0, %ymm0, %ymm0\n"
     "  vpor   %ymm0, %ymm0, %ymm0\n"
     "  vzeroupper\n"
     "  ret\n"
     "  vporzmm_vz:\n"
     "  vpord   %zmm0, %zmm0, %zmm0\n"
     "  vpord   %zmm0, %zmm0, %zmm0\n"
     "  vpord   %zmm0, %zmm0, %zmm0\n"
     "  vpord   %zmm0, %zmm0, %zmm0\n"
     "  vzeroupper\n"
     "  ret\n"
     );
}


void* __attribute__ ((noinline)) dummyMemcpy(void* dest, const void* src, size_t sz) {
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
void* __attribute__ ((noinline)) my_memcpy_64_avx(void* dest, const void* src, size_t sz) {
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
void* __attribute__ ((noinline)) my_memcpy_64_avx512(void* dest, const void* src, size_t sz) {
  __asm__ __volatile__
    (
     "  vmovdqu64    (%rsi),%zmm0\n"
     "  vmovdqu64    %zmm0,(%rdi)\n"
     );
}

// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_erm_movsb(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  mov        %rdx,%rcx\n"
     "  rep        movsb %ds:(%rsi),%es:(%rdi)\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x256_avx512_nt_prefetch(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  prefetcht0 0x200(%rsi)\n"
     "  prefetcht0 0x240(%rsi)\n"
     "  prefetcht0 0x280(%rsi)\n"
     "  prefetcht0 0x2c0(%rsi)\n"
     "  prefetcht0 0x300(%rsi)\n"
     "  prefetcht0 0x340(%rsi)\n"
     "  prefetcht0 0x380(%rsi)\n"
     "  prefetcht0 0x3c0(%rsi)\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  vmovdqu64  0x40(%rsi),%zmm1\n"
     "  vmovdqu64  0x80(%rsi),%zmm2\n"
     "  vmovdqu64  0xc0(%rsi),%zmm3\n"
     "  add        $0x100,%rsi\n"
     "  sub        $0x100,%rdx\n"
     "  vmovntdq   %zmm0,(%rdi)\n"
     "  vmovntdq   %zmm1,0x40(%rdi)\n"
     "  vmovntdq   %zmm2,0x80(%rdi)\n"
     "  vmovntdq   %zmm3,0xc0(%rdi)\n"
     "  add        $0x100,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* my_memcpy_x256_avx512(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  vmovdqu64  0x40(%rsi),%zmm1\n"
     "  vmovdqu64  0x80(%rsi),%zmm2\n"
     "  vmovdqu64  0xc0(%rsi),%zmm3\n"
     "  add        $0x100,%rsi\n"
     "  sub        $0x100,%rdx\n"
     "  vmovdqu64  %zmm0,(%rdi)\n"
     "  vmovdqu64  %zmm1,0x40(%rdi)\n"
     "  vmovdqu64  %zmm2,0x80(%rdi)\n"
     "  vmovdqu64  %zmm3,0xc0(%rdi)\n"
     "  add        $0x100,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x256_avx512_nt(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  vmovdqu64  0x40(%rsi),%zmm1\n"
     "  vmovdqu64  0x80(%rsi),%zmm2\n"
     "  vmovdqu64  0xc0(%rsi),%zmm3\n"
     "  add        $0x100,%rsi\n"
     "  sub        $0x100,%rdx\n"
     "  vmovntdq   %zmm0,(%rdi)\n"
     "  vmovntdq   %zmm1,0x40(%rdi)\n"
     "  vmovntdq   %zmm2,0x80(%rdi)\n"
     "  vmovntdq   %zmm3,0xc0(%rdi)\n"
     "  add        $0x100,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x128_avx_nt_prefetch(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  prefetcht0 0x100(%rsi)\n"
     "  prefetcht0 0x140(%rsi)\n"
     "  prefetcht0 0x180(%rsi)\n"
     "  prefetcht0 0x1c0(%rsi)\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    0x20(%rsi),%ymm1\n"
     "  vmovdqu    0x40(%rsi),%ymm2\n"
     "  vmovdqu    0x60(%rsi),%ymm3\n"
     "  add        $0x80,%rsi\n"
     "  sub        $0x80,%rdx\n"
     "  vmovntdq   %ymm0,(%rdi)\n"
     "  vmovntdq   %ymm1,0x20(%rdi)\n"
     "  vmovntdq   %ymm2,0x40(%rdi)\n"
     "  vmovntdq   %ymm3,0x60(%rdi)\n"
     "  add        $0x80,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x128_avx_nt(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    0x20(%rsi),%ymm1\n"
     "  vmovdqu    0x40(%rsi),%ymm2\n"
     "  vmovdqu    0x60(%rsi),%ymm3\n"
     "  add        $0x80,%rsi\n"
     "  sub        $0x80,%rdx\n"
     "  vmovntdq   %ymm0,(%rdi)\n"
     "  vmovntdq   %ymm1,0x20(%rdi)\n"
     "  vmovntdq   %ymm2,0x40(%rdi)\n"
     "  vmovntdq   %ymm3,0x60(%rdi)\n"
     "  add        $0x80,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x128_avx(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    0x20(%rsi),%ymm1\n"
     "  vmovdqu    0x40(%rsi),%ymm2\n"
     "  vmovdqu    0x60(%rsi),%ymm3\n"
     "  add        $0x80,%rsi\n"
     "  sub        $0x80,%rdx\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,0x20(%rdi)\n"
     "  vmovdqu    %ymm2,0x40(%rdi)\n"
     "  vmovdqu    %ymm3,0x60(%rdi)\n"
     "  add        $0x80,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}



// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x128_avx512_nt_prefetch(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  prefetcht0 0x100(%rsi)\n"
     "  prefetcht0 0x140(%rsi)\n"
     "  prefetcht0 0x180(%rsi)\n"
     "  prefetcht0 0x1c0(%rsi)\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  vmovdqu64  0x40(%rsi),%zmm1\n"
     "  add        $0x80,%rsi\n"
     "  sub        $0x80,%rdx\n"
     "  vmovntdq   %zmm0,(%rdi)\n"
     "  vmovntdq   %zmm1,0x40(%rdi)\n"
     "  add        $0x80,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x128_avx512(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  vmovdqu64  0x40(%rsi),%zmm1\n"
     "  add        $0x80,%rsi\n"
     "  sub        $0x80,%rdx\n"
     "  vmovdqu64  %zmm0,(%rdi)\n"
     "  vmovdqu64  %zmm1,0x40(%rdi)\n"
     "  add        $0x80,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* my_memcpy_x128_avx512_nt(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  vmovdqu64  0x40(%rsi),%zmm1\n"
     "  add        $0x80,%rsi\n"
     "  sub        $0x80,%rdx\n"
     "  vmovntdq   %zmm0,(%rdi)\n"
     "  vmovntdq   %zmm1,0x40(%rdi)\n"
     "  add        $0x80,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}



// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x64_avx_nt_prefetch(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  prefetcht0 0x80(%rsi)\n"
     "  prefetcht0 0xc0(%rsi)\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    0x20(%rsi),%ymm1\n"
     "  add        $0x40,%rsi\n"
     "  sub        $0x40,%rdx\n"
     "  vmovntdq   %ymm0,(%rdi)\n"
     "  vmovntdq   %ymm1,0x20(%rdi)\n"
     "  add        $0x40,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x64_avx_nt(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    0x20(%rsi),%ymm1\n"
     "  add        $0x40,%rsi\n"
     "  sub        $0x40,%rdx\n"
     "  vmovntdq   %ymm0,(%rdi)\n"
     "  vmovntdq   %ymm1,0x20(%rdi)\n"
     "  add        $0x40,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}




// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x64_avx(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu    (%rsi),%ymm0\n"
     "  vmovdqu    0x20(%rsi),%ymm1\n"
     "  add        $0x40,%rsi\n"
     "  sub        $0x40,%rdx\n"
     "  vmovdqu    %ymm0,(%rdi)\n"
     "  vmovdqu    %ymm1,0x20(%rdi)\n"
     "  add        $0x40,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x64_avx512_nt_prefetch(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  prefetcht0 0x80(%rsi)\n"
     "  prefetcht0 0xc0(%rsi)\n"
     "  vmovdqu64    (%rsi),%zmm0\n"
     "  add        $0x40,%rsi\n"
     "  sub        $0x40,%rdx\n"
     "  vmovntdq   %zmm0,(%rdi)\n"
     "  add        $0x40,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x64_avx512_nt(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  prefetcht0 0x80(%rsi)\n"
     "  prefetcht0 0xc0(%rsi)\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  add        $0x40,%rsi\n"
     "  sub        $0x40,%rdx\n"
     "  vmovntdq   %zmm0,(%rdi)\n"
     "  add        $0x40,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}




// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_x64_avx512(void* dest, const void* src, size_t sz) {
/*
*/
  __asm__ __volatile__
    (
     "  1:\n"
     "  vmovdqu64  (%rsi),%zmm0\n"
     "  add        $0x40,%rsi\n"
     "  sub        $0x40,%rdx\n"
     "  vmovdqu64  %zmm0,(%rdi)\n"
     "  add        $0x40,%rdi\n"
     "  cmp        $0x0,%rdx\n"
     "  ja         1b\n"
     "  sfence\n"
     );
}


// caller must make sure the address alligned to 64 bytes
void* __attribute__ ((noinline)) my_memcpy_64_avx_unroll_10(void* dest, const void* src, size_t sz) {
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



void core_test(size_t block_size, size_t off1, size_t off2, size_t gap, unsigned long long int itr1) {
	unsigned long long delta1 = 0, delta2 = 0;
	unsigned long long i = 0;
	register char *m1 = NULL, *m2 = NULL;
	register char *p1 = NULL, *p2 = NULL;
	unsigned long long start_ns;
	struct timespec ts;
	struct rusage r1, r2;
	size_t to_alloc = 0;
	unsigned long long int itr2 = 10;

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

	unsigned long long magic = 1234;
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
	unsigned long long repeat2 = 100UL;
	// default case
	my_memcpy = memcpy;

	while ((opt = getopt(argc, argv, "s:mx:y:lg:r:p:c:")) != -1) {
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
		case 'p':
			repeat2 = strtoul(optarg, NULL, 0);
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
		case 6491:
			my_memcpy = my_memcpy_64_avx;
			break;
		case 6492:
			my_memcpy = my_memcpy_64_avx512;
			break;
		case 6401:
			my_memcpy = my_memcpy_x64_avx_nt_prefetch;
			break;
		case 6402:
			my_memcpy = my_memcpy_x64_avx_nt;
			break;
		case 6403:
			my_memcpy = my_memcpy_x64_avx;
			break;
		case 6411:
			my_memcpy = my_memcpy_x64_avx512_nt_prefetch;
			break;
		case 6412:
			my_memcpy = my_memcpy_x64_avx512_nt;
			break;
		case 6413:
			my_memcpy = my_memcpy_x64_avx512;
			break;
		case 12801:
			my_memcpy = my_memcpy_x128_avx_nt_prefetch;
			break;
		case 12802:
			my_memcpy = my_memcpy_x128_avx_nt;
			break;
		case 12803:
			my_memcpy = my_memcpy_x128_avx;
			break;
		case 12811:
			my_memcpy = my_memcpy_x128_avx512_nt_prefetch;
			break;
		case 12812:
			my_memcpy = my_memcpy_x128_avx512_nt;
			break;
		case 12813:
			my_memcpy = my_memcpy_x128_avx512;
			break;
		case 25611:
			my_memcpy = my_memcpy_x256_avx512_nt_prefetch;
			break;
		case 25612:
			my_memcpy = my_memcpy_x256_avx512_nt;
			break;
		case 25613:
			my_memcpy = my_memcpy_x256_avx512;
			break;
		case 3:
			my_memcpy = my_memcpy_64_avx_unroll_10;
			break;
		case 4:
			my_memcpy = my_memcpy_erm_movsb;
			break;
		case 5:
			//printf("Repeat %llu\n", repeat);
			my_delay_wrapper(repeat, repeat2);
			exit(0);
		case 6:
			//printf("Repeat %llu\n", repeat);
			my_lat_measure_wrapper(repeat, repeat2);
			exit(0);

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

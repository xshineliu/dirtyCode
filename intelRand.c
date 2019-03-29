//gcc -shared -fPIC  intelrand.c -O3 -o intelrand.so -mrdrnd

#include <immintrin.h>
#include <stdlib.h>

#define _GNU_SOURCE

long int random(void){
	unsigned long long rnd;
	__builtin_ia32_rdrand64_step(&rnd);
	return (long int)(rnd & 0x7FFFFFF);
}


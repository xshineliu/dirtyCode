// gcc -O3 -o mmap_behavior mmap_behavior.c

#define _GNU_SOURCE
#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>


#define BUFLEN (256)

typedef enum {
	size = 0,
	kps,
	mmu,
	rss,
	pss,
	shrc,
	shrd,
	pric,
	prid,
	ref,
	anon,
	lzf,
	anonp,
	shmap,
	shhu,
	prhu,
	swap,
	swpss,
	lockd,
	FIELD_LEN,
} field_type;

char *(field_name_short[]) = { "size", "kps", "mmu", "rss", "pss", "shrc",
		"shrd", "pric", "prid", "ref", "anon", "lzf", "anonp", "shmap", "shhu",
		"prhu", "swap", "swpss", "lockd", };

char *(field_name[]) = { "Size:", "KernelPageSize:", "MMUPageSize:", "Rss:",
		"Pss:", "Shared_Clean:", "Shared_Dirty:", "Private_Clean:",
		"Private_Dirty:", "Referenced:", "Anonymous:", "LazyFree:",
		"AnonHugePages:", "ShmemPmdMapped:", "Shared_Hugetlb:",
		"Private_Hugetlb:", "Swap:", "SwapPss:", "Locked:", };


int fields[FIELD_LEN];
char smap_file_name[1024];

int parse(char *filename) {
	int c, i, j;
	char linebuf[BUFSIZ];
	FILE *file;

	file = fopen(filename, "r");
	if (!file) {
		perror(filename);
		return 1;
	}

	memset(fields, 0, FIELD_LEN * sizeof(int));


	char _name[BUFSIZ];
	int _value;

	while (fgets(linebuf, sizeof(linebuf), file)) {

		//fprintf(stderr, "%s", linebuf);
		if (ferror(file)) {
			perror(filename);
			break;
		}

		memset(_name, 0, sizeof(_name));
		if (sscanf(linebuf, "%s%d", _name, &_value) != 2) {
			continue;
		}

		//fprintf(stderr, "*** %s %d\n", _name, _value);

		for (j = 0; j < FIELD_LEN; j++) {
			if (strcmp(_name, field_name[j]) == 0) {
				fields[j] += _value;
				break;
			}
		}

	}
	fclose(file);

	//for (j = 0; j < FIELD_LEN; j++) {
	//	fprintf(stderr, "*** %s %d\n", field_name[j], fields[j]);
	//}

}

#define EXPECT(thing) if (!(thing)) { perror("Failed "#thing); exit(1); }
#define MAX_PAGE_IN (1024 * 1024 * 128)
#define MIN(a,b) ((a)<(b)?(a):(b))

static void inline get_rss(const char *tag) {
	parse(smap_file_name);
	printf("%-25s ==>\t", tag); fflush(stdout);
}

static void inline get_rss_basic(const char *tag) {
	struct rusage u1;
	int ret = 0;
	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	parse(smap_file_name);
	printf("%-25s ==>\t%s:\t%lu\n", tag, field_name[rss], fields[rss]); fflush(stdout);
}


int main(int argc, const char **argv) {

	size_t mysize = 128 * 1024 * 1024;
	size_t pgsize = sysconf(_SC_PAGESIZE);

	int fd = -1;
	int ret = -1;

	int madv_free_supported = 1;

	struct rusage u1, u2;
	memset(&u1, 0, sizeof(u1));
	memset(&u2, 0, sizeof(u2));
	unsigned long long clk1 = 0;
	unsigned long long clk2 = 0;

	pid_t mypid = getpid();
	sprintf(smap_file_name, "/proc/%d/smaps", mypid);

	// map it
	get_rss_basic("before mmap                    ");
	void *addr = NULL;
	void *map = mmap(addr, mysize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, fd, 0);
	get_rss_basic("after mmap                     ");
	EXPECT(map != MAP_FAILED);
	printf("Addr at:\t%p\n", map);

	// does WILLNEED do anything?  seems like no.
	ret = madvise(map, mysize, MADV_WILLNEED);
	EXPECT(ret == 0);
	((unsigned char*)map)[0] = '\0';
	printf("sleep 1 second ... please wait ... \n");
      	fflush(stdout);
	sleep(1);
	get_rss_basic("after WILLNEED                 ");

	int i, limit;
	limit = MIN(mysize, MAX_PAGE_IN);

	// walk it.  add it to sum and print at the end so the optimizer doesn't get clever.
	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	clk1 = __builtin_ia32_rdtsc();
	for (i = 0; i < limit; ) {
		((unsigned char*)map)[i] = '\0';
		i += pgsize;
	}
	clk2 = __builtin_ia32_rdtsc();
	ret = getrusage(RUSAGE_SELF, &u2);
	EXPECT(ret == 0);
	get_rss("after touching                 ");
	printf("%s:\t%lu\tru_minflt:\t%llu\tclks:\t%llu\n", field_name[rss], fields[rss], u2.ru_minflt - u1.ru_minflt, clk2 - clk1);

	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	clk1 = __builtin_ia32_rdtsc();
	for (i = 0; i < limit; ) {
		((unsigned char*)map)[i] = '\0';
		i += pgsize;
	}
	clk2 = __builtin_ia32_rdtsc();
	ret = getrusage(RUSAGE_SELF, &u2);
	EXPECT(ret == 0);
	get_rss("Re-touching                    ");
	printf("%s:\t%lu\tru_minflt:\t%llu\tclks:\t%llu\n", field_name[rss], fields[rss], u2.ru_minflt - u1.ru_minflt, clk2 - clk1);


#ifndef MADV_FREE
#define MADV_FREE 8
#warning ("Warning : MADV_FREE NOT DEFINED, FORCE DEFINE to " MADV_FREE)
#endif
	// give it back!
	// here are other flags you can pass to madvise:
	clk1 = __builtin_ia32_rdtsc();
	ret = madvise(map, mysize, MADV_FREE);
	clk2 = __builtin_ia32_rdtsc();
	if(ret != 0) {
		madv_free_supported = 0;
		fprintf(stderr, " *** MADV_FREE is not supported for this kernel\n");
	}
	printf("\tclks of MADV_FREE:\t%llu\n", clk2 - clk1);
	get_rss_basic("after FREE                     ");


	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	clk1 = __builtin_ia32_rdtsc();
	for (i = 0; i < limit; ) {
		((unsigned char*)map)[i] = '\0';
		i += pgsize;
	}
	clk2 = __builtin_ia32_rdtsc();
	ret = getrusage(RUSAGE_SELF, &u2);
	EXPECT(ret == 0);
	get_rss("after madv_free re-touching    ");
	printf("%s:\t%lu\tru_minflt:\t%llu\tclks:\t%llu\n", field_name[rss], fields[rss], u2.ru_minflt - u1.ru_minflt, clk2 - clk1);


	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	clk1 = __builtin_ia32_rdtsc();
	for (i = 0; i < limit; ) {
		((unsigned char*)map)[i] = '\0';
		i += pgsize;
	}
	clk2 = __builtin_ia32_rdtsc();
	ret = getrusage(RUSAGE_SELF, &u2);
	EXPECT(ret == 0);
	get_rss("Re-touching                    ");
	printf("%s:\t%lu\tru_minflt:\t%llu\tclks:\t%llu\n", field_name[rss], fields[rss], u2.ru_minflt - u1.ru_minflt, clk2 - clk1);


	// give it back!
	// here are other flags you can pass to madvise:
	clk1 = __builtin_ia32_rdtsc();
	madvise(map, mysize, MADV_FREE);
	clk2 = __builtin_ia32_rdtsc();
	printf("\tclks of MADV_FREE:\t%llu\n", clk2 - clk1);
	get_rss_basic("after secondary FREE           ");

	// give it back!
	// here are other flags you can pass to madvise:
	// MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED, MADV_DONTNEED
	clk1 = __builtin_ia32_rdtsc();
	ret = madvise(map, mysize, MADV_DONTNEED);
	clk2 = __builtin_ia32_rdtsc();
	EXPECT(ret == 0);
	printf("\tclks of DONTNEED:\t%llu\n", clk2 - clk1);
	get_rss_basic("after DONTNEED                 ");

	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	clk1 = __builtin_ia32_rdtsc();
	for (i = 0; i < limit; ) {
		((unsigned char*)map)[i] = '\0';
		i += pgsize;
	}
	clk2 = __builtin_ia32_rdtsc();
	ret = getrusage(RUSAGE_SELF, &u2);
	EXPECT(ret == 0);
	get_rss("after madv_dontneed re-touching");
	printf("%s:\t%lu\tru_minflt:\t%llu\tclks:\t%llu\n", field_name[rss], fields[rss], u2.ru_minflt - u1.ru_minflt, clk2 - clk1);


	ret = getrusage(RUSAGE_SELF, &u1);
	EXPECT(ret == 0);
	clk1 = __builtin_ia32_rdtsc();
	for (i = 0; i < limit; ) {
		((unsigned char*)map)[i] = '\0';
		i += pgsize;
	}
	clk2 = __builtin_ia32_rdtsc();
	ret = getrusage(RUSAGE_SELF, &u2);
	EXPECT(ret == 0);
	get_rss("Re-touching                    ");
	printf("%s:\t%lu\tru_minflt:\t%llu\tclks:\t%llu\n", field_name[rss], fields[rss], u2.ru_minflt - u1.ru_minflt, clk2 - clk1);

	// give it back!
	// here are other flags you can pass to madvise:
	// MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED, MADV_DONTNEED
	clk1 = __builtin_ia32_rdtsc();
	ret = madvise(map, mysize, MADV_DONTNEED);
	clk2 = __builtin_ia32_rdtsc();
	EXPECT(ret == 0);
	printf("\tclks of DONTNEED:\t%llu\n", clk2 - clk1);
	get_rss_basic("after direct DONTNEED          ");

	return 0;
}

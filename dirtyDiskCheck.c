#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#define BUF_SIZE_BIT_SHIFT (12)
#define BUF_SIZE  (1 << BUF_SIZE_BIT_SHIFT)
#define GIB_BIT_SHIFT (30)
#define N4KGIB (1024 * 256)
#define MAX_COUNT (4 * N4KGIB)

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <time.h>

#include <libaio.h>

#define NBYTES 4096
#define DEF_DEPTH   16

inline int die_no_mem(int hit, int lineno) {
	if (hit) {
		perror("No Mem");
		fprintf(stderr, "Exit from line %d\n", lineno);
		exit(-lineno);
	}
}

inline int die_general(int hit, int lineno, io_context_t *pctx) {
	if (hit) {
		perror("Error");
		fprintf(stderr, "Exit from line %d\n", lineno);

		if(pctx != NULL) {
			io_destroy(*pctx);
		}

		exit(-lineno);
	}
}

double time_us(struct timespec* const ts) {
	if (clock_gettime(CLOCK_REALTIME, ts)) {
		perror("clock_gettime Error");
		exit(1);
	}
	return ((double) ts->tv_sec) * 1000000.f + (double) ts->tv_nsec / 1000.f;
}

int doAIOTest(const char* filename, int depth) {

	int fd, rc, got, j, k, nbytes = NBYTES, maxevents = DEF_DEPTH;

	void *ptr = NULL;
	/* GNU C extention */
	char *buf[depth];
	float lat[depth], avg = 0.0f;
	/* no need to zero iocbs; will be done in io_prep_pread */
	struct iocb iocbray[depth], *iocb, *iocbp[depth];
	/* need clear? */
	struct io_event events[2 * depth];
	off_t offset;
	io_context_t ctx = 0;

	struct timespec timeout = { 10, 0 };

	struct timespec now_ns;
	double start_us;
	double delta;

	long long unsigned int rndval;
	size_t nr_4k;

	//printf("opening %s\n", filename);

	/* notice opening with these flags won't hurt a device node! */

	if ((fd = open(filename, O_RDONLY | O_DIRECT,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)) < 0) {
		printf("couldn't open %s, ABORTING\n", filename);
		exit(-1);
	}

	size_t file_size_in_bytes = 0;
	// option another method
	// off_t file_size_in_bytes = lseek(fd, 0, SEEK_END);
	ioctl(fd, BLKGETSIZE64, &file_size_in_bytes);
	nr_4k = file_size_in_bytes / NBYTES;

	posix_memalign(&ptr, 4096, NBYTES * depth);
	die_no_mem((ptr == NULL), __LINE__);

	/* write initial data out, clear buffers, allocate iocb's */

	for (j = 0; j < depth; j++) {
		buf[j] = (char *) (ptr + NBYTES * j);
	}

	/* prepare the context */
	rc = io_setup(maxevents, &ctx);
	die_general((rc < 0), __LINE__, &ctx);

	/* (async) read the data from the file */

	for (j = 0; j < depth; j++) {
		lat[j] = 0.0f;

		iocb = &iocbray[j];
		__builtin_ia32_rdrand64_step(&rndval);
		offset = rndval % nr_4k;
		offset *= NBYTES;
		io_prep_pread(iocb, fd, (void *) buf[j], NBYTES, offset);
		// !!! io_prep_pread have zeroed *iocb
		iocb->data = (void *) (intptr_t) j;
		iocbp[j] = iocb;
		//rc = io_submit(ctx, 1, &iocb);
		//die_general((rc != 1), __LINE__, &ctx);
	}

	rc = io_submit(ctx, depth, iocbp);
	die_general((rc != depth), __LINE__, &ctx);

	start_us = time_us(&now_ns);

	/* sync up and print out the readin data */
	got = 0;
	while (1) {
		rc = io_getevents(ctx, 1, DEF_DEPTH, events, &timeout);
		die_general((rc < 1), __LINE__, &ctx);

		delta = time_us(&now_ns) - start_us;
		if (rc > 0) {
			got += rc;
			//printf(" rc from io_getevents on the read = %d\n", rc);
			for (k = 0; k < rc; k++) {
				j = (int) (intptr_t) events[k].data;
				lat[j] = delta;
				//printf("%i: Got %d ptr=%016p %d\n", k, j,
				//		events.obj, ((struct iocb *)(events.obj))->data);
			}

			if (got >= depth) {
				break;
			}
		}

		// timeout
		if (delta > 10000000.0f) {
			fprintf(stderr, "%s time out with %.03f, got %d out of %d\n",
					__func__, delta, got, depth);
			printf("Error: -1\n");
			exit(-1);
		}
	}

	for (k = 0; k < depth; k++) {
		fprintf(stderr, "%d: %.03f\t", k, lat[k]);
		avg += lat[k];
	}

	fprintf(stderr, "\n");
	printf("Success: %.03f\n", avg / depth);

	/* clean up */
	rc = io_destroy(ctx);
	close(fd);
}

int main(int argc, char *argv[]) {
	char* filename = "/dev/sda";

	/* open or create the file and fill it with a pattern */

	if (argc > 1) {
		filename = argv[1];
	}

	doAIOTest(filename, DEF_DEPTH);

	exit(0);
}

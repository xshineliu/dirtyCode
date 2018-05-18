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



struct ioTestResult {
	int n;
	float res[];
};


struct SharedMsg {
	io_context_t ctx;
	int elems;
	struct io_event *e;
	struct iocb *io;
	struct iocb* *ios;
	void* *buf;
	struct timespec ts;
};

double time_us(struct timespec* const ts) {
	if (clock_gettime(CLOCK_REALTIME, ts)) {
		perror("clock_gettime Error");
		exit(1);
	}
	return ((double) ts->tv_sec) * 1000000.f
		+ (double) ts->tv_nsec / 1000.f;
}


size_t getDiskSize(const char* path) {
	size_t file_size_in_bytes = 0;
	int fd = open(path, O_RDONLY | O_DIRECT);
	if(fd < 0) {
		perror("File not exists");
		printf("Error: 0\n");
		exit(1);
	}
	// option another method
	// off_t file_size_in_bytes = lseek(fd, 0, SEEK_END);
	ioctl(fd, BLKGETSIZE64, &file_size_in_bytes);
	close(fd);
	return file_size_in_bytes;
}

struct ioTestResult *blkIOTest(const char* path) {
	struct SharedMsg msg;
	io_context_t *ctx = NULL;
	int pagesize = sysconf(_SC_PAGESIZE);


	int depth = 16; /* default io depth */
	int batch = depth;
	int got = 0;
	int fd = -1;
	int ret = -1;
	int i = 0, j = 0;
	size_t pos = 0;
	size_t nr_4k = getDiskSize(path) / BUF_SIZE;
	unsigned long long rndval = 0;


	struct timespec now_ns;
	double start_us;
	double delta;
	double avg = -1;

	void *ptr = NULL;


	if(nr_4k < 1) {
		return NULL;
	}

	struct ioTestResult *myRes = calloc(1, sizeof(struct ioTestResult) +
			depth* sizeof(float));
	// TODO check

	memset(&msg, 0, sizeof(struct SharedMsg));

	ctx = &(msg.ctx);

	if (io_setup(depth, ctx) != 0) {
		fprintf(stderr, "%s: io_setup error\n", __func__);
		printf("Error: 0\n");
		exit(3);
	}

	fd = open(path, O_RDONLY);
	if(fd < 0) {
			err("File not exist.\n");
			io_destroy(*ctx);
			printf("Error: 0\n");
			exit(3);
	}

	msg.ts.tv_sec = 10;
	msg.ts.tv_nsec = 0;
	msg.e = calloc(depth, sizeof(struct io_event));
	msg.io = calloc(depth, sizeof(struct iocb));
	msg.ios = calloc(depth, sizeof(struct iocb*));
	msg.buf = calloc(depth, sizeof(void *));

	// TODO check
	if (myRes == NULL || msg.e == NULL || msg.io == NULL || msg.ios == NULL || msg.buf == NULL) {
		fprintf(stderr, "No memory\n");
		printf("Error: 0\n");
		exit(0);
	}

	for (i = 0; i < depth; i++) {
		ret = posix_memalign(&ptr, pagesize, BUF_SIZE);
		if (ret != 0) {
			fprintf(stderr, "No memory\n");
			printf("Error: 0\n");
			exit(0);
		}
		memset(ptr, 0, BUF_SIZE);
		msg.buf[i] = ptr;
		//msg.io[i].data = (void *)(intptr_t) i;

		msg.ios[i] = msg.io + i;

		__builtin_ia32_rdrand64_step(&rndval);
		pos = rndval % nr_4k;

		io_prep_pread(msg.ios[i], fd, msg.buf[i], BUF_SIZE, pos);
		/* Must setup after io_prep_pread */
		msg.io[i].data = (void *)(intptr_t) i;
		//printf("%i: Set iocb=%016p ptr=%016p pos=%016x %x\n",
		//		i, msg.ios[i], msg.buf[i], pos, msg.io[i].data);
	}

	start_us = time_us(&now_ns);

	if ((ret = io_submit(*ctx, batch, msg.ios)) != batch) {
		io_destroy(*ctx);
		// TODO free memory ...
		fprintf(stderr, "%s io_submit of %d request error: ret=%d\n",
				__func__, batch, ret);
		printf("Error: 0\n");
		exit(4);
	}

	got = 0;
	while(1) {
		ret = io_getevents(*ctx, 1, depth, msg.e, &(msg.ts));
		if (ret < 1) {
			perror("ret < 1");
			// TIMEOUT
			fprintf(stderr, "%s TIME OUT %d\n", msg.ts.tv_sec);
			printf("Error: -1\n");
			exit(-1);
		}

		delta = time_us(&now_ns) - start_us;

		for (i = 0; i < ret; i++) {
			j = (int) (intptr_t) msg.e[i].data;
			myRes->res[j] = delta;
			got++;
			//printf("%i: Got %d ptr=%016p %d\n", i, j,
			//		msg.e[i].obj, ((struct iocb *)(msg.e[i].obj))->data);
		}

		if (got >= batch) {
			break;
		}

		if (delta > 10000000.0f) {
			fprintf(stderr, "%s time out with %.03f, got %d out of %d\n",
					__func__, delta, got, batch);
			//break;
			printf("Error: -1\n");
			exit(-1);
		}
	}

	myRes->n = got;
	// TODO memory clean up

	for(i = 0; i < batch; i++) {
		avg += myRes->res[i];
		fprintf(stderr, "%d: %.03f\t", i, myRes->res[i]);
	}
	fprintf(stderr, "\n");
	printf("Success: %.03f\n", avg / batch);
	return myRes;

}



int main(int argc, char* argv[]) {

	const char *dev = "/dev/sda";
	if(argc > 1) {
		dev = argv[1];
	}
	//fprintf(stdout, "%s: %.03f GiB\n", dev, (double)getDiskSize(dev)/1024.0f/1024.0f/1024.0f);

	blkIOTest(dev);
	return 0;

}

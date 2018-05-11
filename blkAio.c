/* gcc -o blkaio blockAio.c -mrdrnd -laio */

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <libaio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

//#define MAX_COUNT (1024 * 1024)
#define BUF_SIZE_BIT_SHIFT (12)
#define GIB_BIT_SHIFT (30)
#define N4KGIB (1024 * 256)
#define MAX_COUNT (4 * N4KGIB)
#define BUF_SIZE  (1 << BUF_SIZE_BIT_SHIFT)

#ifndef O_DIRECT
#define O_DIRECT         040000 /* direct disk access hint */
#endif

#define NTHREAD 2
#define NDEPS 16

pthread_t tid[NTHREAD];
int debug = 0;
int delay = 0; // in micro second
int rnd = 0;
int depth = NDEPS;
int fsize = (MAX_COUNT / N4KGIB);
int ops = MAX_COUNT;
int nblock = MAX_COUNT;

struct StatData {
	unsigned long long nr_submit;
	unsigned long long nr_checking;
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



double time_ns(struct timespec* const ts) {
	if (clock_gettime(CLOCK_REALTIME, ts)) {
		perror("clock_gettime Error");
		exit(1);
	}
	return ((double) ts->tv_sec) * 1000000.f
		+ (double) ts->tv_nsec / 1000.f;
}


static void print_help(const char *progname)
{
	printf("Usage: %s [OPTS] fname\n", progname);
	printf("-d	delay in micro seconds\n");
	printf("-l	iodeps\n");
	printf("-s	file size in GiB (at least 1 GiB)\n");
	printf("-n	number of request\n");
	printf("-r	use random [default not]\n");
	printf("-v	debug level\n");
	printf("-f	filename\n");
}


inline void update_pos(unsigned long long *ppos, int rndway) {
	unsigned int rndval = 0;
	if(rndway) {
		__builtin_ia32_rdrand32_step(&rndval);
		*ppos = ((unsigned long long)(rndval % nblock) << BUF_SIZE_BIT_SHIFT );
	} else {
		*ppos += BUF_SIZE;
		*ppos %= ((unsigned long long)fsize << GIB_BIT_SHIFT);
	}
}


int main(int argc, char *argv[]) {
	int fd;
	unsigned long long pos = 0;
	struct SharedMsg msg;
	struct StatData stat_data;
	char *filename = "/dev/sda";

	void* ptr = NULL;
	io_context_t *ctx = NULL;
	//struct iocb *io = NULL;
	int opt, i, j, n, batch, got, ret;
	struct timespec now_ns;
	double start_ns;
	double delta;

	int pagesize = sysconf(_SC_PAGESIZE);


	while ((opt = getopt(argc, argv, "d:l:s:n:v:rf:")) != -1) {
		switch (opt) {	
		case 'd':
			delay = strtoul(optarg, NULL, 0);
			break;	
		case 'l':
			depth = strtoul(optarg, NULL, 0);
			break;	
		case 's':
			fsize = strtoul(optarg, NULL, 0);
			nblock = fsize * N4KGIB;
			ops = nblock;
			break;	
		case 'n':
			ops = strtoul(optarg, NULL, 0);
			break;	
		case 'v':
			debug = strtoul(optarg, NULL, 0);
			break;	
		case 'r':
			rnd = 1;
			break;
		case 'f':
			filename = optarg;
			break;
		default:
			print_help(argv[0]);
			return 0;
		}
	}
	
	memset(&msg, 0, sizeof(struct SharedMsg));
	memset(&stat_data, 0, sizeof(struct StatData));

	msg.ts.tv_sec = 5;
	msg.ts.tv_nsec = 0;
	ctx = &(msg.ctx);

	if (io_setup(depth, ctx) != 0) {
		printf("io_setup error\n");
		return -1;
	}

	if ((fd = open(filename, O_RDONLY | O_DIRECT, 0644))
			< 0) {
		perror("open error");
		io_destroy(*ctx);
		return -1;
	}


	msg.e = calloc(depth, sizeof(struct io_event));
	msg.io = calloc(depth, sizeof(struct iocb));
	msg.ios = calloc(depth, sizeof(struct iocb*));
	msg.buf = calloc(depth, sizeof(void *));

	for (i = 0; i < depth; i++) {
		posix_memalign(&ptr, pagesize, BUF_SIZE);
		memset(ptr, 0, BUF_SIZE);
		msg.buf[i] = ptr;
		msg.io[i].data = (void *)(intptr_t) i;
		msg.ios[i] = msg.io + i;

		io_prep_pread(msg.ios[i], fd, msg.buf[i], BUF_SIZE, pos);
		//pos += BUF_SIZE;
		update_pos(&pos, rnd);
	}


	start_ns = time_ns(&now_ns);

	n = ops;
	got = 0;

	batch = depth;
	if(n < depth) {
		batch = n;
	}

	if ((ret = io_submit(*ctx, batch, msg.ios)) != batch) {
		io_destroy(*ctx);
		printf("io_submit of %d request error: ret=%d\n", batch, ret);
		return -1;
	}
	stat_data.nr_submit++;
	n -= batch;

	while (got < ops) {
		if(delay > 0 && delay < 1000000) {
			usleep(delay);
		}

		ret = io_getevents(*ctx, 1, depth, msg.e, NULL);
		stat_data.nr_checking++;
		if (ret < 1) {
			perror("ret < 1");
			return -1;
		}

		for (i = 0; i < ret; i++) {
			j = (int) (intptr_t) msg.e[i].data;
			got++;
			if (got >= ops) {
				break;
			}


			// submit next if not finished submit
			if(n <= 0) {
				// no more to submit, but continue to checking result
				continue;
			}
			//msg.io[j].data = (void *) j;
			io_prep_pread(msg.ios[j], fd, msg.buf[j], BUF_SIZE, pos);
			io_submit(*ctx, 1, msg.ios + j);
			stat_data.nr_submit++;
			n--;
			//pos += BUF_SIZE;
			update_pos(&pos, rnd);
		}
		// n may == 0 here
	}

	close(fd);
	io_destroy(*ctx);

	delta = time_ns(&now_ns) - start_ns;

	printf("ops=%d, got=%d, n=%d.\n", ops, got, n);
	printf("submit\tchecking\ttime\tBW\n");
	printf("%lld\t%lld\t%.6f\t%.3f\n", stat_data.nr_submit, stat_data.nr_checking, delta/1000.f, 
		(double)ops * (double)BUF_SIZE / (delta / 1000000.f) / 1024.f / 1024.f );

	return 0;
}

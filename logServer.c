#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <stddef.h>  /* size_t */
#include <string.h>  /* memset, etc */
#include <stdlib.h>  /* exit */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NR_MAX_THREADS 64
#define MAX_CONN_PER_THREAD 40960
#define FLUSH_DURATION_DEFAULT 240
#define FLUSH_DURATION_VARIABLE 60
#define NR_MAX_POLL 8192
#define TEST_IDX 0
#define EXIT_DELAY 1
#define NET_READ_BUF (4 * 1024) // 4 KiB
#define NET_READ_BUF_MAX (32 * 1024) // 32 KiB
#define DEFAULT_DATA_BLOCK_SIZE (64 * 1024) // 64 KiB
#define STEP_DATA_BLOCK_SIZE (8 * 1024) // 8 KiB
#define MAX_DATA_BLOCK_SIZE (128 * 1024) // 128 KiB
#define EXT_DATA_BLOCK_SIZE (32 * 1024 * 1024) // 32 MiB

#define REALLOC_LAT 180
#define TZ_OFFSET 8

#define MAX_DIR_NUM 32
#define LOG_DIR "log"
#define DEFAULT_DIR_PREFIX "/data%02d/%s/%s/%d_%03d/%s/%s.log"
#define MONTHLY_DIR_PREFIX "/data%02d/%s/%s/%s/%s.log"

#define BASE_DISCONN_SECS 86400
#define VARI_DISCONN_SECS 86400

int conn_slot_lock[NR_MAX_THREADS] = { 0, };

#define LOCK(a) while(!__sync_bool_compare_and_swap(&a, 0, 1)) {;}
#define UNLOCK(a) __sync_lock_release(&a);

int goToStop = 0;

struct thread_data {
	int idx;
	int logfd;
};

struct gstatistic {
	struct tm cur_tminfo;
	time_t cur_time;
	char date_str[32];
	unsigned long long ticks; /* uptime in seconds        */
	unsigned long long last_report_tick;
	unsigned long long last_dir_set_tick;

	unsigned long long disk_io_bytes[NR_MAX_THREADS];
	unsigned long long net_in_bytes[NR_MAX_THREADS];
	unsigned long long last_disk_io_bytes[NR_MAX_THREADS];
	unsigned long long last_net_in_bytes[NR_MAX_THREADS];

	unsigned long long disk_io_threads_busy_loops;
	unsigned long long last_disk_io_threads_busy_loops;

	int snapshot_session_alive;
	int period_succ_submit_disk_io_timer_expire_forced;
	int period_succ_submit_disk_io;
	int period_succ_finished_disk_io;
	int period_failed_finished_disk_io;
	int cur_seq;

#ifdef DIR_SWITCH_MONTHLY
	int monthly_id;
#endif

	unsigned long long failed_disk_bytes_write;

};

struct gstatistic gstats;


struct {
	int nWorkers; /* number of threads configured  */
	int nr_dir;
	int port; /* local port to listen on  */
	int fd; /* listener descriptor      */

	in_addr_t addr; /* local IP or INADDR_ANY   */

	void *session_array[NR_MAX_THREADS]; /* point to sessions array */
	int curr_nr_conn[NR_MAX_THREADS];

	int signal_fd; /* used to receive signals  */
	int epoll_fd[NR_MAX_THREADS + 1]; /* used for all notification*/
	int diskw_odirect;
	int verbose;
	int disk_thread_delay_scan_ms;
	int force_disconnect_seconds;

	int dir_switch_period;
	int pid; /* our own pid              */
	char log_idf[32];
	char *prog;
} cfg = { .nWorkers = 1, .nr_dir = 1,
		.addr = INADDR_ANY, /* by default, listen on all local IP's   */
		.fd = -1, .signal_fd = -1, .epoll_fd = { -1, }, .dir_switch_period = 600};

/* signals that we'll accept synchronously via signalfd */
int sigs[] = { SIGIO, SIGHUP, SIGTERM, SIGINT, SIGQUIT, SIGALRM };

struct session {
	// init val should be NULL
	void *disk_buf;
	// init val should be buf1
	void *cur_buf;

	void *buf1;
	void *buf2;

	int time_out_need_flush_to_disk;
	int buf_full_stuck;

	volatile int disk_io_ops_lock;
	volatile int disk_io_in_progress;

	int threadidx;
	// len to read
	int net_read_buf_len;
	// len to write
	int disk_write_buf_len;
	// flag of the delete stage
	int in_delete_stage;

	// ipv4 only now
	int ip;
	int flush_duration;

	int events;
	int socket_fd;
	int on_disk_fd;
	int membuf_offset;

	int slot_idx;
	int need_delay_free;

	int mem_buf_len;
	int file_seq;

#ifdef DIR_SWITCH_MONTHLY
	int monthly_seq;
#endif

	unsigned long long start_ticks;
	unsigned long long last_disk_io_write_tick;

	unsigned long long network_bytes;
	unsigned long long disk_bytes;
};


#ifdef DIR_SWITCH_MONTHLY
int get_month_seq(int *val){

	int myval = 0;
	time_t rawtime;
	struct tm timeinfo;
	char buffer[32];

	time(&rawtime);
	localtime_r(&rawtime, &timeinfo);

	strftime (buffer, 3, "%m", &timeinfo);
	myval = atoi(buffer);
	*val = myval;

	return 0;
}
#endif


/* should call with lock locked per thread */
int get_free_conn_slot(int thread_idx) {
	int i = 0;
	struct session **sptr = cfg.session_array[thread_idx];

	for (i = 0; i < MAX_CONN_PER_THREAD; i++) {
		if (sptr[i] == NULL) {
			return i;
		}
	}
	return -1;
}

int mkpath(char* file_path, mode_t mode) {
	assert(file_path && *file_path);
	char* p;
	for (p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
		*p = '\0';
		if (mkdir(file_path, mode) == -1) {
			if (errno != EEXIST) {
				*p = '/';
				return -1;
			}
		}
		*p = '/';
	}
	return 0;
}

/* must be called by get_new_session, or safe point when switch to new file */
int session_set_on_disk_log_filename(struct session *s) {
	int fd = -1;

	char fileName[128];
	char date_str1[16];
	//char date_str2[16];
	char ip_str1[16];
	char ip_str2[16];
	unsigned char *p;

	int ip = s->ip;

	p = (unsigned char *) &ip;
	sprintf(ip_str1, "%03u_%03u_%03u_%03u", p[3] & 0xFF, p[2] & 0xFF,
			p[1] & 0xFF, 0);
	sprintf(ip_str2, "%03u_%03u_%03u_%03u", p[3] & 0xFF, p[2] & 0xFF,
			p[1] & 0xFF, p[0] & 0xFF);


#ifdef DIR_SWITCH_MONTHLY
	strftime(date_str1, 7, "%Y%m", &(gstats.cur_tminfo));
	sprintf(fileName, MONTHLY_DIR_PREFIX, p[0] % cfg.nr_dir + 1,
			cfg.log_idf, date_str1, ip_str1, ip_str2);
#else
	strftime(date_str1, 9, "%Y%m%d", &(gstats.cur_tminfo));
	//strftime(date_str2, 13, "%Y%m%d%H%M", &(gstats.cur_tm));
	sprintf(fileName, DEFAULT_DIR_PREFIX, p[0] % cfg.nr_dir + 1,
			cfg.log_idf, date_str1, (86400 / cfg.dir_switch_period),
			gstats.cur_seq, ip_str1, ip_str2);
#endif


	if (mkpath(fileName, 0755)) {
		fprintf(stderr, "[ERROR] File %s can not setup upper directory: %s\n",
				fileName, strerror(errno));
		return -1;
	}

	fd = open(fileName, O_CREAT | O_WRONLY | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	// O_DIRECT need buf offset and len align, relax to use posix_fadvice

	if(cfg.diskw_odirect) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE);
	}

	if (fd < 0) {
		fprintf(stderr, "[ERROR] File %s can not be opened for appending: %s\n",
				fileName, strerror(errno));
		return -1;
	}

	// if open successful, switch to the new fd, else relies on the old fd
	// TODO: retry control

	/* close old fd (switch to new date dir) in case last fd is not -1 */
	if (s->on_disk_fd >= 0) {
		close(s->on_disk_fd);
		//s->on_disk_fd = -1;
	}
	// switch to new on-disk fd
	s->on_disk_fd = fd;
	return fd;
}

struct session* get_new_session(int ip, int events, int socket_fd, int tid) {

	struct session* s = calloc(1, sizeof(struct session));
	if (s == NULL) {
		return NULL;
	}

	s->buf1 = calloc(2, DEFAULT_DATA_BLOCK_SIZE);
	if (s->buf1 == NULL) {
		free(s);
		return NULL;
	}

	s->net_read_buf_len = NET_READ_BUF;
	s->mem_buf_len = DEFAULT_DATA_BLOCK_SIZE;
	s->buf2 = s->buf1 + DEFAULT_DATA_BLOCK_SIZE;
	s->cur_buf = s->buf1;
	s->disk_buf = NULL;
	s->in_delete_stage = 0;
	s->need_delay_free = 0;

	s->events = events;
	s->threadidx = tid;
	s->ip = ip;
	s->socket_fd = socket_fd;

	// must set to -1, as 0 is meaningful
	s->on_disk_fd = -1;
	// s->slot_idx is set in the last step of get session
	s->slot_idx = -1;

	s->flush_duration = FLUSH_DURATION_DEFAULT + ip % FLUSH_DURATION_VARIABLE;
	s->start_ticks = gstats.ticks;
	s->file_seq = gstats.cur_seq;

#ifdef DIR_SWITCH_MONTHLY
	get_month_seq(&(s->monthly_seq));
#endif

	// delay to call session_set_on_disk_log_filename, when need to write to disk
	return s;
}

/* mark session in delete, and handle network fd */
int free_session_step1(struct session* s) {

	int rc = 0;
	struct epoll_event ev;
	int idx = s->threadidx;
	int sfd = s->socket_fd;

	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_DEL, sfd, &ev);
	if (rc == -1) {
		fprintf(stderr, "[ERROR] %s epoll_ctl: %s\n", __func__,
				strerror(errno));
	}
	close(sfd);
	s->socket_fd = -1;

	if (cfg.verbose > 0) {
		int ip = s->ip;
		char *p = (char *) &ip;
		fprintf(stderr, "[NOTICE] %s thread %d client %u.%u.%u.%u with sfd %d closed, "
				"up %d, mbuf %p len %d size %d, dbuf %p len %d fd %d, nin %ld dout %ld "
				"nin_rate %d, pending %d, dio_flag %d\n",
				gstats.date_str, idx, p[3] & 0xff, p[2] & 0xff, p[1] & 0xff, p[0] & 0xff, sfd,
				gstats.ticks - s->start_ticks, s->cur_buf, s->membuf_offset, s->mem_buf_len,
				s->disk_buf, s->disk_write_buf_len, s->on_disk_fd, s->network_bytes,
				s->disk_bytes, s->network_bytes / (gstats.ticks - s->start_ticks + 1),
				s->network_bytes - s->disk_bytes, s->disk_io_in_progress);
	}

	s->in_delete_stage = 10;
	return rc;
}

/* judge weather need delay */
void free_session_step2(struct session *s) {

	// will be free in disk write threads
	if (s->membuf_offset > 0 && (s->slot_idx > -1)) {
		LOCK(s->disk_io_ops_lock);


		// if there is no io thread (and no pending write), update fields, else do nothing
		if (s->disk_buf == NULL) {
			// at this time, lock held, io_in_progress is 1, or io done with io_in_progress cleared
			s->disk_write_buf_len = s->membuf_offset;
			s->disk_buf = s->cur_buf;
			// tell IO thread to clear in-use resource, may be these is case missing
			// so need another threads to do clean up these orphans
			s->need_delay_free = 1;
			// s->membuf_offset set to 0 to indicate no dirty data left
			s->membuf_offset = 0;
			UNLOCK(s->disk_io_ops_lock);

			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] ^^^ delay write %d bytes at %p for fd %d on thread %d slot %d due to dirty mem buf\n",
						s->disk_write_buf_len, s->disk_buf, s->on_disk_fd, s->threadidx, s->slot_idx);
			}


		} else {
			s->need_delay_free = 1;
			// there is pending io, io_in_progress must be 0, yet not started to set s->disk_buf to NULL
			UNLOCK(s->disk_io_ops_lock);

			// s->membuf_offset keep gt 0 with s->need_delay_free with 1 as an indicate a 2nd pass write

			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] ^^^ delay write %d bytes + %d bytes at %p for fd %d on thread %d slot %d due to"
						"2 pending IO,  io_in_progress %d\n", s->disk_write_buf_len, s->membuf_offset,
						s->cur_buf, s->on_disk_fd, s->threadidx, s->slot_idx, s->disk_io_in_progress);
			}

		}

		// remain will be free and close in io thread
	}

	// else : disk_fd could be closed here, but delayed ???

	s->in_delete_stage = 20;
}


/* this rotine does not handle network fd */
int free_session_res(struct session* s) {

	// get_new_seesion will not set s->on_disk_fd
	if(s->on_disk_fd >= 0) {
		close(s->on_disk_fd);
		s->on_disk_fd = -1;
	}

	if (s->buf1) {

		if(cfg.verbose > 1) {
			fprintf(stderr, "[DEBUG] %s free s=%p buf1=%p dbuf=%p\n", __func__, s, s->buf1, s->disk_buf);
		}

		free(s->buf1);
		s->buf1 = NULL;
	}

	s->in_delete_stage = 30;
	return 0;
}


// mainly for free struct session, and maybe reset cfg.session_array[tidx][slot] if slot got
// the last step, only accept_client and disk_write_thread can call this function, not allowed for drain_client
void free_session_final(struct session *s) {
	int pos, thread_idx;
	struct session **sptr;

	if (!s) {
		return;
	}

	// actually, no use for the assignment, just for readable
	s->in_delete_stage = 40;

	thread_idx = s->threadidx;
	pos = s->slot_idx;
	if(cfg.verbose > 1) {
		fprintf(stderr, "[DEBUG] %s free %d/%d s=%p buf1=%p dbuf=%p\n",
				__func__, thread_idx, pos, s, s->buf1, s->disk_buf);
	}
	free(s);

	// lock not needed
	// pos non -1 means at setup stage, slot allocated already, else do nothing
	if (pos > -1) {
		__atomic_fetch_add(&(cfg.curr_nr_conn[thread_idx]), -1, __ATOMIC_SEQ_CST);
		sptr = cfg.session_array[thread_idx];
		sptr[pos] = NULL;
	}

}



// del_session will close network connection, but maybe not close disk fd
int del_session(struct session* s, int immediately) {
	int idx = s->threadidx;
	int sfd = s->socket_fd;

	int rc = free_session_step1(s);

	//TODO FIXME
	// will be free in disk write threads
	if (s->membuf_offset > 0 && (s->slot_idx > -1)) {
		LOCK(s->disk_io_ops_lock);
		// if there is no io thread (and no pending write), update fields, else do nothing
		if (s->disk_buf == NULL) {
			// at this time, lock held, io_in_progress is 1, or io done with io_in_progress cleared
			s->disk_write_buf_len = s->membuf_offset;
			s->disk_buf = s->cur_buf;
			// tell IO thread to clear in-use resource, may be these is case missing
			// so need another threads to do clean up these orphans
			s->need_delay_free = 1;
			// s->membuf_offset set to 0 to indicate no dirty data left
			s->membuf_offset = 0;
			UNLOCK(s->disk_io_ops_lock);

			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] ^^^ delay write %d bytes at %p for fd %d on thread %d slot %d\n",
						s->disk_write_buf_len, s->disk_buf, sfd, idx, s->slot_idx);
			}
		} else {
			s->need_delay_free = 1;
			// there is pending io, io_in_progress must be 0, yet not started to set s->disk_buf to NULL
			UNLOCK(s->disk_io_ops_lock);

			// s->membuf_offset keep gt 0 with s->need_delay_free with 1 as an indicate a 2nd pass write

			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] ^^^ delay write %d bytes at %p for fd %d on thread %d due to pending IO, "
						"io_in_progress %d\n", s->disk_write_buf_len, s->cur_buf, sfd, idx, s->disk_io_in_progress);
			}

		}

		// remain will be free and close in io thread
		return 0;
	}

	// s->membuf_offset == 0 || s->slot_idx == -1
	// there is not delay pending action to consume the s->cur_buf, do all the clean up
	// free_session_res(s) && free_session_final(s) will be called delayed in another thread as in_delete_stage marked
	// but could be forced by immediately
	free_session_res(s);
	if(immediately) {
		free_session_final(s);
	}
	return rc;
}


void usage() {
	fprintf(stderr, "usage: %s [-v] [-a <ip>] -p <port>\n", cfg.prog);
	exit(-1);
}

/* do periodic work here */
void periodic() {
	int i;
	double disk_io_bytes[NR_MAX_THREADS];
	double net_in_bytes[NR_MAX_THREADS];
	unsigned long long diff_ticks = 0;
	unsigned long long busy_poll_loops = 0;

	// must go first, as cur_time as used to cal gstats.cur_seq
	time(&gstats.cur_time);
	localtime_r(&gstats.cur_time, &gstats.cur_tminfo);
	strftime(gstats.date_str, 32, "%Y-%m-%d %H:%M:%S", &gstats.cur_tminfo);

	gstats.ticks++;
	// update every second
	gstats.cur_seq = ((long)gstats.cur_time + 3600 * TZ_OFFSET) % (long)86400 / (long)cfg.dir_switch_period;


	if ((gstats.ticks % 10) != 0) {
		return;
	}

#ifdef DIR_SWITCH_MONTHLY
	// at most delay 1 min
	if ((gstats.ticks % 60) == 0) {
		get_month_seq(&gstats.monthly_id);
	}
#endif

	// print every 10s
	diff_ticks = gstats.ticks - gstats.last_report_tick;
	gstats.last_report_tick = gstats.ticks;

	busy_poll_loops = gstats.disk_io_threads_busy_loops
			- gstats.last_disk_io_threads_busy_loops;
	gstats.last_disk_io_threads_busy_loops = gstats.disk_io_threads_busy_loops;

	fprintf(stdout, "%s up_secs %ld failed_disk_write_bytes %ld\n",
			gstats.date_str, gstats.ticks, gstats.failed_disk_bytes_write);
	fprintf(stdout, "         time_stamp up_secs  alive    force_wr total_wr succ_fin failed_f polls_ps\n");
	fprintf(stdout, "%s % 8d % 8d % 8.2f % 8.2f % 8.2f % 8.2f % 8.2f\n\n",
			gstats.date_str, gstats.ticks, gstats.snapshot_session_alive,
			(double)gstats.period_succ_submit_disk_io_timer_expire_forced / (double)diff_ticks,
			(double)gstats.period_succ_submit_disk_io / (double)diff_ticks,
			(double)gstats.period_succ_finished_disk_io / (double)diff_ticks,
			(double)gstats.period_failed_finished_disk_io / (double)diff_ticks,
			(double) busy_poll_loops
					/ ((double) gstats.snapshot_session_alive + .001f)
					/ (double) diff_ticks);

	gstats.period_succ_submit_disk_io_timer_expire_forced = 0;
	gstats.period_succ_submit_disk_io = 0;
	gstats.period_succ_finished_disk_io = 0;
	gstats.period_succ_finished_disk_io = 0;

	for (i = 0; i < cfg.nWorkers; i++) {
		fprintf(stdout, "%d (% 3d)\t", cfg.curr_nr_conn[i], i);
	}
	fprintf(stdout, "\n\n");

	if (cfg.verbose > 1) {
		fprintf(stdout, "Net In IO KiBytes Total\n");
		for (i = 0; i < cfg.nWorkers; i++) {
			fprintf(stdout, "%.03f (% 3d)\t", gstats.net_in_bytes[i] / 1024.0f,
					i);
		}
		fprintf(stdout, "\n");

		fprintf(stdout, "Disk IO KiBytes Total\n");
		for (i = 0; i < cfg.nWorkers; i++) {
			fprintf(stdout, "%.03f (% 3d)\t", gstats.disk_io_bytes[i] / 1024.0f,
					i);
		}
		fprintf(stdout, "\n");

		fprintf(stdout, "Net In IO KiBytes last tick\n");
		for (i = 0; i < cfg.nWorkers; i++) {
			fprintf(stdout, "%.03f (% 3d)\t",
					(double) (gstats.net_in_bytes[i]
							- gstats.last_net_in_bytes[i]) / 1024.0f, i);
		}
		fprintf(stdout, "\n");

		fprintf(stdout, "Disk IO KiBytes Total last tick\n");
		for (i = 0; i < cfg.nWorkers; i++) {
			fprintf(stdout, "%.03f (% 3d)\t",
					(double) (gstats.disk_io_bytes[i]
							- gstats.last_disk_io_bytes[i]) / 1024.0f, i);
		}
		fprintf(stdout, "\n\n");

	}

	fprintf(stdout, "Net In IO KiBytes per second\n");
	for (i = 0; i < cfg.nWorkers; i++) {
		net_in_bytes[i] = (double) (gstats.net_in_bytes[i]
				- gstats.last_net_in_bytes[i]);
		gstats.last_net_in_bytes[i] = gstats.net_in_bytes[i];
		fprintf(stdout, "%.03f (% 3d)\t",
				net_in_bytes[i] / 1024.0f / (double) diff_ticks, i);
	}
	fprintf(stdout, "\n");

	fprintf(stdout, "Disk IO KiBytes per second\n");
	for (i = 0; i < cfg.nWorkers; i++) {
		disk_io_bytes[i] = (double) (gstats.disk_io_bytes[i]
				- gstats.last_disk_io_bytes[i]);
		gstats.last_disk_io_bytes[i] = gstats.disk_io_bytes[i];
		fprintf(stdout, "%.03f (% 3d)\t",
				disk_io_bytes[i] / 1024.0f / (double) diff_ticks, i);
	}
	fprintf(stdout, "\n\n\n");

	fflush(stdout);
}

int add_epoll_to_session(struct session* s) {
	int rc;
	struct epoll_event ev;
	int idx = s->threadidx;
	int fd = s->socket_fd;
	int events = s->events;

	memset(&ev, 0, sizeof(ev)); // placate valgrind
	ev.events = events;
	ev.data.ptr = s;
	if (cfg.verbose > 1)
		fprintf(stderr, "[INFO] adding fd %d thread idx %d to epoll\n", fd,
				idx);
	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_ADD, fd, &ev);
	if (rc == -1) {
		fprintf(stderr, "%s epoll_ctl: %s\n", __func__, strerror(errno));
	}
	return rc;
}

int add_epoll(int events, int fd, int idx) {
	int rc;
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev)); // placate valgrind
	ev.events = events;
	ev.data.fd = fd;
	if (cfg.verbose > 1) {
		fprintf(stderr, "[INFO] adding fd %d thread idx %d to epoll\n", fd,
				idx);
	}
	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_ADD, fd, &ev);
	if (rc == -1) {
		fprintf(stderr, "[ERROR] %s epoll_ctl: %s\n", __func__,
				strerror(errno));
	}
	return rc;
}






int del_epoll(int fd, int idx) {
	int rc;
	struct epoll_event ev;
	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_DEL, fd, &ev);
	if (rc == -1) {
		fprintf(stderr, "[ERROR] epoll_ctl: %s\n", strerror(errno));
	}
	return rc;
}

int setup_listener() {
	int rc = -1, one = 1;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		fprintf(stderr, "[ERROR] socket: %s\n", strerror(errno));
		goto done;
	}

	/**********************************************************
	 * internet socket address structure: our address and port
	 *********************************************************/
	struct sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cfg.addr;
	sin.sin_port = htons(cfg.port);

	/**********************************************************
	 * bind socket to address and port
	 *********************************************************/
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr*) &sin, sizeof(sin)) == -1) {
		fprintf(stderr, "[ERROR] bind: %s\n", strerror(errno));
		goto done;
	}

	/**********************************************************
	 * put socket into listening state
	 *********************************************************/
	if (listen(fd, 8192) == -1) {
		fprintf(stderr, "[ERROR] listen: %s\n", strerror(errno));
		goto done;
	}

	cfg.fd = fd;
	rc = 0;

	done: if ((rc < 0) && (fd != -1))
		close(fd);
	return rc;
}

/* accept a new client connection to the listening socket */
int accept_client() {

	int sfd, slot_idx, succ = 0;
	struct sockaddr_in in;
	socklen_t sz = sizeof(in);
	int thread_idx = 0;
	int hash = 0;
	struct session *s = NULL, **sptr = NULL;

	sfd = accept(cfg.fd, (struct sockaddr*) &in, &sz);
	if (sfd == -1) {
		fprintf(stderr, "[ERROR] accept: %s\n", strerror(errno));
		goto done;
	}

	hash = ntohl(in.sin_addr.s_addr) & 0xFF;
	thread_idx = hash % cfg.nWorkers;

	//int get_new_session(int ip, int events, int socket_fd, int tid)
	s = get_new_session(ntohl(in.sin_addr.s_addr), EPOLLIN, sfd, thread_idx);
	if (s == NULL) {
		close(sfd);
		sfd = -1;
		goto done;
	}

	// s != NULL
	if ((add_epoll_to_session(s) != -1)) {
		LOCK(conn_slot_lock[thread_idx]);
		slot_idx = get_free_conn_slot(thread_idx);
		if (slot_idx < 0) {
			// not found free slot
			UNLOCK(conn_slot_lock[thread_idx]);
			if (cfg.verbose > 0) {
				fprintf(stderr,
						"[INFO] worker threads %d connection table full\n",
						thread_idx);
			}
			// succ remains 0
		} else {
			// found free slot, fill the slot
			sptr = (struct session **) (cfg.session_array[thread_idx]);
			sptr[slot_idx] = s;
			__atomic_fetch_add(&(cfg.curr_nr_conn[thread_idx]), 1, __ATOMIC_SEQ_CST);
			s->slot_idx = slot_idx;
			UNLOCK(conn_slot_lock[thread_idx]);
			// memory barrier of unlock
			succ = 1;
		}
	}

	// add_epoll_to_session(s) may be -1 here, succ remains zero
	// get_free_conn_slot may fail here, succ remains zero

	done:
	if (cfg.verbose > 0) {
		fprintf(stderr,
				"[INFO] connection fd %d from %s:%d hash %d and assigned to thread %d, rc=%d\n",
				sfd, inet_ntoa(in.sin_addr), (int) ntohs(in.sin_port), hash,
				thread_idx, succ);

		if (succ) {
			int ip = s->ip;
			char *p = (char *) &ip;
			fprintf(stderr, "[NOTICE] %s added %u.%u.%u.%u session pointer %p to slot %d, "
					"thread %d, buf1 %p buf2 %p, dir seq %d, flush period %d\n",
					gstats.date_str, p[3] & 0xff, p[2] & 0xff, p[1] & 0xff, p[0] & 0xff, s, slot_idx,
					s->threadidx, s->buf1, s->buf2, s->file_seq, s->flush_duration);
		}
	}

	if(succ) {
		return sfd;
	}

	// if fd > 0, s will not null, del_session will close sfd in step1
	// delete_session_ptr called in del_session if no delay action is need, and curr_nr_conn[i] non NULL
	if (s) {
		// succ not true, that means s not in session_array]
		free_session_step1(s);
		// step 2 not necessary
		free_session_res(s);
		free_session_final(s);
	}
	return -1;
}


int write2disk(struct session *s) {

	int rc = 0;
	int towrite = 0;
	int ntries = 100;
	char *buf = NULL;
	int nleft = 0;

	if (!s) {
		// warn
		if (cfg.verbose > 2) {
			fprintf(stderr, "[ERROR] !!! %s parameter error s !!!\n", __func__);
		}
		return -1;
	}

	if (s->disk_io_in_progress) {
		if (cfg.verbose > 2) {
			fprintf(stderr, "[ERROR] !!! %s previous write not finish !!!\n",
					__func__);
		}
		return -1;
	}

	if (!(s->disk_buf)) {
		// warn
		if (cfg.verbose > 2) {
			fprintf(stderr, "[ERROR] !!! %s parameter error buf !!!\n",
					__func__);
		}
		return -1;
	}

	// announce i'm working now
	LOCK(s->disk_io_ops_lock);
	s->disk_io_in_progress = 1;
	buf = s->disk_buf;
	// prevent further disk IO scheduling
	s->disk_buf = NULL;
	UNLOCK(s->disk_io_ops_lock);

	nleft = s->disk_write_buf_len;
	towrite = nleft;

	if (cfg.verbose > 3) {
		fprintf(stderr, "[DEBUG] *** %s enter write to disk process "
				"with buf ptr %p and len %d\n", __func__, buf, towrite);
	}

	while (ntries > 0 && nleft > 0) {
		rc = write(s->on_disk_fd, buf, nleft);
		if (rc < -1) {
			if (errno == EINTR) {
				continue;
			}
			// warn
			ntries--;
			continue;
		}
		nleft -= rc;
	}


	if(nleft) {
		//warn
		gstats.failed_disk_bytes_write += nleft;
	}

	LOCK(s->disk_io_ops_lock);
	s->disk_write_buf_len = 0;
	s->disk_io_in_progress = 0;
	// clear anyway
	s->buf_full_stuck = 0;
	UNLOCK(s->disk_io_ops_lock);

	s->disk_bytes += (towrite - nleft);
	return towrite - nleft;
}

int disk_write_thread(void *data) {
	int thread_idx, slot_idx, rc, sfd, tid;
	struct session *s = NULL, **sptr;

	int stat_session_alive = 0;
	int stat_succ_submit_disk_io_timer_expire_forced = 0;
	int stat_succ_submit_disk_io = 0;
	int stat_succ_finished_disk_io = 0;
	int stat_failed_finished_disk_io = 0;

	while (1) {
		gstats.snapshot_session_alive = stat_session_alive;
		__atomic_fetch_add(
				&(gstats.period_succ_submit_disk_io_timer_expire_forced),
				stat_succ_submit_disk_io_timer_expire_forced, __ATOMIC_SEQ_CST);
		__atomic_fetch_add(&(gstats.period_succ_submit_disk_io),
				stat_succ_submit_disk_io, __ATOMIC_SEQ_CST);
		__atomic_fetch_add(&(gstats.period_succ_finished_disk_io),
				stat_succ_finished_disk_io, __ATOMIC_SEQ_CST);
		__atomic_fetch_add(&(gstats.period_failed_finished_disk_io),
				stat_failed_finished_disk_io, __ATOMIC_SEQ_CST);

		stat_session_alive = 0;
		stat_succ_submit_disk_io_timer_expire_forced = 0;
		stat_succ_submit_disk_io = 0;
		stat_succ_finished_disk_io = 0;
		stat_failed_finished_disk_io = 0;

		for (thread_idx = 0; thread_idx < cfg.nWorkers; thread_idx++) {
			sptr = cfg.session_array[thread_idx];

			for (slot_idx = 0; slot_idx < MAX_CONN_PER_THREAD; slot_idx++) {
				s = sptr[slot_idx];
				if (!s) {
					continue;
				}

				// after s in session_array, only thread can free the session
				if(s->in_delete_stage == 20) {
					if(s->need_delay_free == 0) {
						// stage 30
						free_session_res(s);
						// stage 40
						free_session_final(s);
						continue;
					}
				}

				gstats.disk_io_threads_busy_loops++;
				stat_session_alive++;

				// close when seq expire, avoid consumer delete before close
				if(s->on_disk_fd >= 0 && (s->file_seq != gstats.cur_seq)) {
					close(s->on_disk_fd);
					s->on_disk_fd = -1;
					// reopen with another seq name when buffer full
				}


				// this step will submit the disk IO
				// if there is a submit to disk io action
				//     time_out_need_flush_to_disk will be clear in pass2write
				// if there is such race condition, pass2write already gets the lock
				//     after it setup properly, our code could run
				// disk_io_in_progress (write2disk) won't be true here, as pass2write will clear it
				// last_disk_io_write_tick will be updated immediately after a successful write2disk call
				// this code must before  s->time_out_need_flush_to_disk set to 1
				//
				if (s->time_out_need_flush_to_disk) {
					LOCK(s->disk_io_ops_lock);
					// as we have got the lock, s->disk_buf can be an indicator
					//              if the buf switch is done in another thread
					// to avoid mistake double switch
					// disk_io_in_progress won't be 1 in this case, as s->time_out_need_flush_to_disk true
					// but add int the code for easy understanding
					if (!(s->disk_buf) && !(s->disk_io_in_progress)) {
						if (s->cur_buf == s->buf1) {
							s->cur_buf = s->buf2;
							s->disk_buf = s->buf1;
						} else {
							s->cur_buf = s->buf1;
							s->disk_buf = s->buf2;
						}
						s->disk_write_buf_len = s->membuf_offset;
						// reset offset to zero
						s->membuf_offset = 0;
					} else {
						//already an io in progress
					}
					UNLOCK(s->disk_io_ops_lock);

					stat_succ_submit_disk_io_timer_expire_forced++;
					// disk IO still waiting, s->last_disk_io_write_tick will be updated until real disk write occured
					// clear the flag, maybe pass2write clear it already, nevertheless
					s->time_out_need_flush_to_disk = 0;

					if (cfg.verbose > 2) {
						fprintf(stderr,
								"[DEBUG] *** S2 force flush submit with lenth %d for fd %d, d %p m %p %p %p\n",
								s->disk_write_buf_len, s->socket_fd, s->disk_buf,
								s->cur_buf, s->buf1, s->buf2);
					}
					// not break the loop, to let next logic submit to io thread
				}

				// this step set s->time_out_need_flush_to_disk to 1, in case
				// s->membuf_offset > 0 and at the same time, timeout occurs
				//              and s->time_out_need_flush_to_disk zero
				// to prevent repeating set to 1,
				// condition must be meet:
				// when next scan to s
				//              conditions may keep (new pkt arrive, leads s->membuf_offset > 0 still true)
				//              s->time_out_need_flush_to_disk already clear to 0, but s->disk_buf is set by
				//              myself thread, or in rare case, by the drain thread
				// io thread will clear s->disk_buf, but set s->disk_io_in_progress
				// io thread ends with s->last_disk_io_write_tick re-calculated
				// transaction finished

				if ((s->membuf_offset > 0)
						&& ((gstats.ticks - s->last_disk_io_write_tick)
								> (s->flush_duration))
						&& !(s->time_out_need_flush_to_disk)
						// in case s->time_out_need_flush_to_disk set to zero again
						//              require s->disk_buf null, and
						&& (s->disk_buf == NULL)
						// in case s->disk_buf set to NULL again
						//              require s->disk_io_in_progress done (not zero)
						&& !(s->disk_io_in_progress)) {
					// do not need lock, as 1->0->1->0 is a logical sequence, and "set 1" only occurs here
					s->time_out_need_flush_to_disk = 1;
					// to avoid race condition logic, sched to next check point

					// TODO random s->flush_duration for next

					if (cfg.verbose > 2) {
						fprintf(stderr,
								"[INFO] *** S1 force flush to disk due to timeout(%lu %lu) with lenth %d"
										" for fd %d, d %p m %p %p %p\n",
								s->last_disk_io_write_tick, gstats.ticks,
								s->disk_write_buf_len, s->socket_fd, s->disk_buf,
								s->cur_buf, s->buf1, s->buf2);
					}

					// to schedule in the next loop
					continue;
				}

				if (s->disk_buf == NULL) {
					continue;
				}

				tid = s->threadidx;
				sfd = s->socket_fd;

				//assert(s->disk_write_len > 0);
				if (s->disk_write_buf_len < 1) {
					fprintf(stderr,
							"[BUG] *** TICK %lu / %lu, thread %d with lenth %d"
									" for fd %d, d %p m %p %p %p; ",
							s->last_disk_io_write_tick, gstats.ticks,
							s->threadidx, s->disk_write_buf_len, s->socket_fd,
							s->disk_buf, s->cur_buf, s->buf1, s->buf2);
					fprintf(stderr,
							"FLAG flush %d io_in_progress %d, IP %08x\n",
							s->time_out_need_flush_to_disk,
							s->disk_io_in_progress, s->ip);
					//TODO DUMP other info
					continue;
				}

				assert(s->threadidx == thread_idx);

				// check if need to write new file
				// gstats.cur_seq update in every second
#ifdef DIR_SWITCH_MONTHLY
				if ((s->on_disk_fd < 0) || (s->monthly_seq != gstats.monthly_id)) {
					int res;
					s->monthly_seq = gstats.monthly_id;
#else
				if ((s->on_disk_fd < 0) || (s->file_seq != gstats.cur_seq)) {
					// if the get the fd not to -1, set successful switch flag
					int res;
					s->file_seq = gstats.cur_seq;
#endif
					// gstats.cur_seq is used in session_set_on_disk_log_filename, s->file_seq as the flag
					res = session_set_on_disk_log_filename(s);

					if (cfg.verbose > 2) {
						fprintf(stderr,
								"[INFO] *** Switch DIR result thread %d at ticks %ld, res: %d\n",
								s->threadidx, gstats.ticks, res);
					}
				}

				stat_succ_submit_disk_io++;
				rc = write2disk(s);
				if (cfg.verbose > 3) {
					//fprintf([INFO] stderr, "*** %p %p: %d, %p, %p, %d\n", s, dptr,
					//              s->threadidx, s->disk_buf, s->cur_buf,
					//              s->disk_write_len);
					fprintf(stderr,
							"[DEBUG] *** thread %d fd %d drain to disk with lenth %d, return %d\n",
							tid, sfd, s->disk_write_buf_len, rc);
				}

				// assert rc == s->disk_write_len
				if (rc > 0) {
					gstats.disk_io_bytes[tid] += rc;
					s->last_disk_io_write_tick = gstats.ticks;
					stat_succ_finished_disk_io++;
				} else {
					stat_failed_finished_disk_io++;
				}


				// free resource if the network connection already closed
				// no lock needed as this an orphan session
				if (s->need_delay_free) {
					// if s->membuf_offset > 0, it show need second time for the disk write
					if(s->membuf_offset > 0) {
						s->disk_buf = s->cur_buf;
						s->disk_write_buf_len = s->membuf_offset;
						// s->membuf_offset set to 0 to indicate no dirty data left
						s->membuf_offset = 0;
						continue;
					}

					// the case s->membuf_offset already zero
					if (cfg.verbose > 0) {
						fprintf(stderr,
								"[DEBUG] ^^^ doing the delay free with length %d for fd %d thread %d/%d ip %08x, "
								"d %p m %p buf1 %p buf2 %p\n",
								s->disk_write_buf_len, s->socket_fd, s->threadidx, s->slot_idx, s->ip,
								s->disk_buf, s->cur_buf, s->buf1, s->buf2);
					}

					free_session_res(s);
					free_session_final(s);
					stat_session_alive--;
					continue;
				}


			} // end per thread
		} // end all thread

		if (goToStop) {
			break;
		}


		if(cfg.disk_thread_delay_scan_ms > 0) {
			usleep(cfg.disk_thread_delay_scan_ms * 1000);
		}
	} // should not return

	return 0;
}

// must be called with s->lock held, and s->disk_io_in_progress not 1
// return 0 on successful
int enlarge_session_buf(struct session *s, int gap) {

	void *newptr = NULL, *oldptr = s->buf1;
	int oldlen = s->mem_buf_len;
	int newlen = s->mem_buf_len + STEP_DATA_BLOCK_SIZE;

	while(newlen < s->mem_buf_len + gap) {
		newlen += STEP_DATA_BLOCK_SIZE;
	}

	if((gstats.ticks - s->start_ticks) < REALLOC_LAT ) {
		// not in steady status
		return 1;
	}

	// disk io pending or in progress, lock held
	if (newlen > MAX_DATA_BLOCK_SIZE) {
		int bw = (int) (s->network_bytes / (gstats.ticks - s->start_ticks));

		// use a large net_read_buf_len
		if(bw > 256 * 1024) {
			s->net_read_buf_len = 32 * 1024;
		}

		if(bw > 512 * 1024) {
			s->net_read_buf_len = 64 * 1024;
		}

		if(bw > 1 * 1024 * 1024) {
			s->net_read_buf_len = 128 * 1024;
		}

		// not allowed, exceeding the max size
		if( bw < 2 * MAX_DATA_BLOCK_SIZE) {
			return 2;
		}

		// limited to 4x bw
		if(newlen > 4 * bw) {
			return 3;
		}

		// if 4x bw gt EXT_DATA_BLOCK_SIZE, not supported. limited to EXT_DATA_BLOCK_SIZE
		if(newlen > EXT_DATA_BLOCK_SIZE) {
			return 4;
		}

		if (cfg.verbose > 0) {
			fprintf(stderr,
				"[INFO] Realloc: TS %d/%d session %d/%d ip %08x buf size to %d, avg incoming rate %d Bytes/s, "
				"lost or pending %ld\n", gstats.ticks, s->start_ticks, s->threadidx, s->slot_idx, s->ip,
				newlen, bw, s->network_bytes - s->disk_bytes - s->disk_write_buf_len);
		}

	}

	newptr = calloc(2, newlen);
	if(!newptr) {
		if (cfg.verbose > 1) {
			fprintf(stderr,
				"[INFO] Realloc: TS %d/%d session %d/%d buf failed, will stuck %08x pkt\n",
				gstats.ticks, s->start_ticks, s->threadidx, s->slot_idx, s->ip);
		}
		// no memory
		return -1;
	}

	s->buf1 = newptr;
	s->mem_buf_len = newlen;
	s->buf2 = newptr + s->mem_buf_len;

	memcpy(s->buf1, s->cur_buf, s->membuf_offset);
	assert(s->disk_buf != NULL);
	memcpy(s->buf2, s->disk_buf, s->disk_write_buf_len);

	s->cur_buf = s->buf1;
	s->disk_buf = s->buf2;

	free(oldptr);

	if (cfg.verbose > 1) {
		fprintf(stderr,
			"[INFO] Realloc: TS %d/%d session %d/%d mbuf %p dbuf %p, len %d for source %08x successful\n",
				gstats.ticks, s->start_ticks, s->threadidx, s->slot_idx,
				s->cur_buf, s->disk_buf, s->mem_buf_len, s->ip);
	}

	// length field keep unchanged
	return 0;

}

// s->lock maybe hold by

int pass2write(struct session *s, void *buf, int len) {

	void* dist_buf = NULL;

	if (!s || !buf || len < 1) {
		return -1;
	}

recheck:
	// there is case timer expire write threads is or will access s->cur_buf and etc
	// there is slot time_out_need_flush_to_disk already 1, but s->cur_buf and etc not updated
	// this is race condition, a write thread is access s->disk_buf
	// all these race condition need be handled

	if ((s->membuf_offset + len + s->net_read_buf_len) > s->mem_buf_len) {

		LOCK(s->disk_io_ops_lock);

		// we need to write data to disk, and switch pointers, but first checking if it is safe

		// check whether s->disk_buf is hold by disk io thread
		// io in submit progress or in progress, s->disk_buf could not be updated
		// disk_buf not NULL, maybe disk_io_in_progress not started yet
		if (s->disk_buf || s->disk_io_in_progress) {

			// io pending not in progress
			if(s->disk_buf) {
				if(enlarge_session_buf(s,
						(s->membuf_offset + len + s->net_read_buf_len - s->mem_buf_len)) != 0) {
					// session buf not enlarged due to various reason
					s->buf_full_stuck = 1;
					UNLOCK(s->disk_io_ops_lock);
					goto setup_copy;

					// logic finished, end with lock still held
				} else {
					// straight forward, as we at least get new STEP_DATA_BLOCK_SIZE bytes
					UNLOCK(s->disk_io_ops_lock);
					goto recheck;
				}
			} else {
				// disk io in progress, s->disk_buf already NULL, but actually memory region in use due to not clone
				UNLOCK(s->disk_io_ops_lock);
				// spin on s->disk_io_in_progress, wait for buf
				// TODO set timeout, get some delay here, rare case?
				while(s->disk_io_in_progress) {
					;
				}
				// unlikely disk_io_in_progress re-enter, speculative skip re-checking :)))
				// restore lock status
				LOCK(s->disk_io_ops_lock);
			}

			// finish process for the case buffer not large
		}


        // lock must be held in this point
		// we are safe to switch buffer ***

		// if s->time_out_need_flush_to_disk == 1;
		// clear the flag, cancel the pending force flush write, as I will fire in this function
		//__sync_val_compare_and_swap (&(s->time_out_need_flush_to_disk), 1, 0);
		if (s->time_out_need_flush_to_disk) {
			s->time_out_need_flush_to_disk = 0;
		}

		// *** after enlarge buffer, it is NOT likely enter this logic, but maybe, nevertheless :)))
		// passed checking, safe to switching buffer
		//		or no io pending or in progress [most cases]
		//		or if io pending, buf enlarged already
		//		or io in progress has been exited during busy waiting
		// write to disk, check switching to which buffer
		if (s->cur_buf == s->buf1) {
			s->cur_buf = s->buf2;
			s->disk_buf = s->buf1;
		} else {
			s->cur_buf = s->buf1;
			s->disk_buf = s->buf2;
		}
		s->disk_write_buf_len = s->membuf_offset;
		// reset offset to zero
		s->membuf_offset = 0;

		UNLOCK(s->disk_io_ops_lock);

		if (cfg.verbose > 3) {
			fprintf(stderr,
					"[DEBUG] *** Buf Full flush to disk (%lu %lu) with lenth %d"
							" for fd %d, d %p m %p %p %p\n",
					s->last_disk_io_write_tick, gstats.ticks, s->disk_write_buf_len,
					s->socket_fd, s->disk_buf, s->cur_buf, s->buf1, s->buf2);
		}

		// no one is access s->cur_buf and s->membuf_offset, safe update [first copy to the new buf]
		// s->membuf_offset 0 here, but for easy code understaning consistence
		dist_buf = s->cur_buf + s->membuf_offset;
		memcpy(dist_buf, buf, len);
		s->membuf_offset += len;

	} else {

setup_copy:
		// if there is pending timer_expire disk io write, pending stage before switch buf ptr
		if (s->time_out_need_flush_to_disk) {
			LOCK(s->disk_io_ops_lock);
			dist_buf = s->cur_buf + s->membuf_offset;
			memcpy(dist_buf, buf, len);
			s->membuf_offset += len;
			UNLOCK(s->disk_io_ops_lock);
		} else {
			// no one is access s->cur_buf and s->membuf_offset, safe update [not the first write]
			dist_buf = s->cur_buf + s->membuf_offset;
			memcpy(dist_buf, buf, len);
			s->membuf_offset += len;
		}
	}

	if (cfg.verbose > 3) {
		fprintf(stderr, "[DEBUG] \t\tthread %d fd buf %p len: %d\n",
				s->threadidx, s->cur_buf, s->membuf_offset);
	}
	return 0;
}

void drain_client(struct session *s) {
	int rc, readOK = 0;
	// GNU extention calloc
	char buf[s->net_read_buf_len];

	int sfd = s->socket_fd;
	int thread_idx = s->threadidx;

	if (s->slot_idx < 0) {
		// return in case the session setup not compelete
		return;
	}

	if (s->buf_full_stuck) {
		if (cfg.verbose > 1) {
			fprintf(stderr,
					"[WRAN] T %d: thread %d/%d ip %08x with fd %d in buf full stuck status, "
							"mem_buf %p (len %d) dis_buf %p (len %d) and io_in_progress %d\n",
					gstats.ticks - s->start_ticks, s->threadidx, s->slot_idx, s->ip, s->socket_fd,
					s->cur_buf, s->membuf_offset, s->disk_buf, s->disk_write_buf_len, s->disk_io_in_progress);
		}
		return;
	}

	rc = read(sfd, buf, sizeof(buf));
	switch (rc) {
	default:
		if (cfg.verbose > 3) {
			fprintf(stderr, "[DEBUG] \tthread %d received %d bytes\n",
					thread_idx, rc);
		}
		readOK = 1;
		s->network_bytes += rc;
		gstats.net_in_bytes[thread_idx] += rc;
		pass2write(s, (void *) buf, rc);
		break;
	case 0:
		if (cfg.verbose > 0) {
			fprintf(stderr, "[INFO] thread %d fd %d closed\n", thread_idx, sfd);
		}
		// !readOK: close the resource
		break;
	case -1:
		if (cfg.verbose > 2) {
			fprintf(stderr, "[DEBUG] thread %d recv: %s\n", thread_idx,
					strerror(errno));
		}
		if (errno == ECONNRESET) {
			// !readOK: close
			break;
		}
		// set readOK to 1 in order to retry, such as EINTR
		readOK = 1;
	}

	if (readOK)
		return;

	/* client closed. log it, tell epoll to forget it, close it */
	free_session_step1(s);
	free_session_step2(s);
	// delete_session_ptr(s) will be handled by del_session, to be delayed
}

int process_thread(void *data) {
	void *dptr;
	int i, ret;
	int thread_idx = (int) (unsigned long) data;
	struct epoll_event ev[NR_MAX_POLL];

	/* sleep 1 */

	while (1) {
		ret = epoll_wait(cfg.epoll_fd[thread_idx], ev, NR_MAX_POLL, -1);
		if (ret < 1) {
			if (errno = -EINTR) {
				continue;
			}
			fprintf(stderr, "[ERROR] thread %d epoll_wait error: %s\n",
					thread_idx, strerror(errno));
			break;
		}

		for (i = 0; i < ret; i++) {
			/* regular POLLIN. handle the particular descriptor that's ready */

			struct session *s = ev[i].data.ptr;

			//assert(ev[i].events & EPOLLIN);
			if(!(ev[i].events & EPOLLIN) || ev[i].events & EPOLLERR) {
				if (cfg.verbose > 0) {
					int ip = s->ip;
					char *p = (char *) &ip;
					fprintf(stderr, "[ERROR] %s thread %d handle POLLIN error for event %x"
							" on cli %u.%u.%u.%u\n", gstats.date_str, thread_idx, ev[i].events,
							p[3] & 0xff, p[2] & 0xff, p[1] & 0xff, p[0] & 0xff);
				}

				free_session_step1(s);
				free_session_step2(s);
				// delete_session_ptr(s) will be handled by del_session, to be delayed
				continue;
			}

			if (cfg.verbose > 3) {
				fprintf(stderr, "[DEBUG] * thread %d handle POLLIN on fd %d\n",
						thread_idx, s->socket_fd);
			}
			drain_client(s);
			// default as continue
		}

		if (goToStop) {
			break;
		}
	}

	// for thread exit
	done:

	// cleanup

	/* on error, let all other threads exit? OK? */
	goToStop = 1;

	fprintf(stderr, "[INFO] thread %d exit\n", thread_idx);
	/* thread exit, should not happen */
	return 0;
}

inline void init_gstat(void) {

	memset(&gstats, 0, sizeof(gstats));

	time(&gstats.cur_time);
	localtime_r(&gstats.cur_time, &gstats.cur_tminfo);
	strftime(gstats.date_str, 32, "%Y-%m-%d %H:%M:%S", &gstats.cur_tminfo);

	gstats.cur_seq = ((long)(gstats.cur_time) + 3600 * TZ_OFFSET) % (long)86400 / (long)cfg.dir_switch_period;
	fprintf(stderr, "[INFO] %s: gstats.cur_seq set to %d (dir_switch_period=%d)\n",
			gstats.date_str, gstats.cur_seq, cfg.dir_switch_period);

#ifdef DIR_SWITCH_MONTHLY
	get_month_seq(&gstats.monthly_id);
#endif
}

int main(int argc, char *argv[]) {

	int n, opt;
	int i, ret;
	struct epoll_event ev[NR_MAX_POLL];
	struct signalfd_siginfo info;

	pthread_t tid;
	char pname[16];

	cfg.nWorkers = 1;
	cfg.prog = argv[0];
	cfg.log_idf[0] = '\0';
	cfg.pid = getpid();
	cfg.disk_thread_delay_scan_ms = 0;
	cfg.force_disconnect_seconds = 0;

	while ((opt = getopt(argc, argv, "vp:a:n:m:l:d:qt:k:h")) != -1) {
		switch (opt) {
		case 'v':
			cfg.verbose++;
			break;
		case 'p':
			cfg.port = atoi(optarg);
			break;
		case 'a':
			cfg.addr = inet_addr(optarg);
			break;
		case 'n':
			cfg.nWorkers = atoi(optarg);
			if (cfg.nWorkers < 1) {
				cfg.nWorkers = 1;
			}
			if (cfg.nWorkers > NR_MAX_THREADS) {
				cfg.nWorkers = NR_MAX_THREADS;
			}
			break;
		case 'm':
			cfg.nr_dir = atoi(optarg);
			if (cfg.nr_dir < 1) {
				cfg.nWorkers = 1;
			}
			if (cfg.nr_dir > MAX_DIR_NUM) {
				cfg.nr_dir = MAX_DIR_NUM;
			}
			break;
		case 'l':
			strncpy(cfg.log_idf, optarg, 31);
			break;
		case 'd':
			cfg.disk_thread_delay_scan_ms = atoi(optarg);
			break;
		case 't':
			cfg.dir_switch_period = atoi(optarg);
			break;
		case 'k':
			cfg.force_disconnect_seconds = atoi(optarg);
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}
	if (cfg.addr == INADDR_NONE)
		usage();
	if (cfg.port == 0)
		usage();

	if(!cfg.log_idf[0]) {
		strncpy(cfg.log_idf, LOG_DIR, 31);
	}

	// less or equal to 0 indicate never disconnect
	if(cfg.force_disconnect_seconds < 0) {
		cfg.force_disconnect_seconds = 0;
	}

	// at least need 10 min of force_disconnect_seconds
	if(cfg.force_disconnect_seconds > 0 && cfg.force_disconnect_seconds < 600) {
		cfg.force_disconnect_seconds = 600;
	}

#ifdef DIR_SWITCH_MONTHLY
	cfg.dir_switch_period = 86400;
#endif

	/* block all signals. we take signals synchronously via signalfd */
	sigset_t all;
	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, NULL);

	/* a few signals we'll accept via our signalfd */
	sigset_t sw;
	sigemptyset(&sw);
	for (n = 0; n < sizeof(sigs) / sizeof(*sigs); n++)
		sigaddset(&sw, sigs[n]);

	init_gstat();

	if (setup_listener())
		goto done;

	/* create the signalfd for receiving signals */
	cfg.signal_fd = signalfd(-1, &sw, 0);
	if (cfg.signal_fd == -1) {
		fprintf(stderr, "[ERROR] signalfd: %s\n", strerror(errno));
		goto done;
	}

	/* cfg.nWorkers + 1 fds */
	for (i = 0; i < cfg.nWorkers + 1; i++) {
		/* set up the epoll instance */
		cfg.epoll_fd[i] = epoll_create(1);
		if (cfg.epoll_fd[cfg.nWorkers] == -1) {
			fprintf(stderr, "[ERROR] epoll: %s\n", strerror(errno));
			goto done;
		}
	}

	/* add descriptors of interest */
	if (add_epoll(EPOLLIN, cfg.fd, cfg.nWorkers))
		goto done;
	// listening socket
	if (add_epoll(EPOLLIN, cfg.signal_fd, cfg.nWorkers))
		goto done;
	// signal socket

	for (i = 0; i < cfg.nWorkers; i++) {

		if (0
				> (pthread_create(&tid, NULL, process_thread,
						(void *) (unsigned long) i))) {
			perror("[ERROR] could not create worker thread");
			goto done;
		}

		if ((cfg.session_array[i] = (void *) calloc(MAX_CONN_PER_THREAD,
				sizeof(void *))) == NULL) {
			perror("[ERROR] could not alloc conn pointer array memory");
			goto done;
		}

		cfg.curr_nr_conn[i] = 0;
		conn_slot_lock[i] = 0;
		sprintf(pname, "net-worker-%03d", i);
		pthread_setname_np(tid, pname);
	}

	fprintf(stderr, "[INFO] %s start with %d worker threads\n", cfg.prog,
			cfg.nWorkers);

	if (0 > pthread_create(&tid, NULL, disk_write_thread, NULL)) {
		perror("could not create IO thread");
		goto done;
	}
	sprintf(pname, "disk-worker");
	pthread_setname_np(tid, pname);

	/*
	 * This is our main loop. epoll for input or signals.
	 */
	alarm(1);
	while (1) {
		ret = epoll_wait(cfg.epoll_fd[cfg.nWorkers], ev, NR_MAX_POLL, -1);

		if (ret < 1) {
			if (errno = -EINTR) {
				continue;
			}
			fprintf(stderr, "[ERROR] Main thread epoll_wait error: %s\n",
					strerror(errno));
			break;
		}

		for (i = 0; i < ret; i++) {

			/* if a signal was sent to us, read its signalfd_siginfo */
			if (ev[i].data.fd == cfg.signal_fd) {
				if (read(cfg.signal_fd, &info, sizeof(info)) != sizeof(info)) {
					fprintf(stderr,
							"[ERROR] failed to read signal fd buffer\n");
					continue;
				}
				switch (info.ssi_signo) {
				case SIGALRM:
					periodic();
					alarm(1);
					continue;
				case SIGHUP:
					fprintf(stderr,
							"[INFO] got signal SIGHUP (sig %d), ignore\n",
							info.ssi_signo);
					continue;
				default: /* exit */
					fprintf(stderr, "[ERROR] got signal %d\n", info.ssi_signo);
					goto done;
				}
			}

			/* regular POLLIN. handle the particular descriptor that's ready */
			assert(ev[i].events & EPOLLIN);
			if (cfg.verbose > 2) {
				fprintf(stderr, "[DEBUG] handle POLLIN on fd %d\n",
						ev[i].data.fd);
			}
			if (ev[i].data.fd == cfg.fd) {
				accept_client();
			} else {
				fprintf(stderr, "[ERROR] Unexpect event and epoll fd %d\n",
						ev[i].data.fd);
			}
		} // poll

		if (goToStop) {
			break;
		}

	} // main loop

	fprintf(stderr, "[ERROR] epoll_wait: %s\n", strerror(errno));

	done: /* we get here if we got a signal like Ctrl-C */
	goToStop = 1;
	/* wait for childs */
	fprintf(stderr, "[INFO] Wait for %d second(s) to exit\n", EXIT_DELAY);
	sleep(EXIT_DELAY);

	// free works

	if (cfg.signal_fd != -1) {
		del_epoll(cfg.signal_fd, cfg.nWorkers);
		close(cfg.signal_fd);
	}

	if (cfg.fd != -1) {
		del_epoll(cfg.fd, cfg.nWorkers);
		close(cfg.fd);
	}

	if (cfg.epoll_fd[cfg.nWorkers] != -1) {
		close(cfg.epoll_fd[cfg.nWorkers]);
	}

	return 0;

}

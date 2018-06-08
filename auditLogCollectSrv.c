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

#define NR_MAX_THREADS 256
#define MAX_CONN_PER_THREAD 8192
#define FLUSH_DURATION_FORCED 300
#define NR_MAX_POLL 128
#define TEST_IDX 0
#define EXIT_DELAY 1
#define NET_READ_BUF (4 * 1024) // 4 KiB
#define DATA_BLOCK_SIZE (256 * 1024) //256 KiB

#define MAX_DIR_NUM 32
#define PATH_PREFIX_MAX_LEN 16
#define DEFAULT_DIR_PREFIX "/data%02d/log/%s/%s/%s/%s.%s.log"

int conn_slot_lock[NR_MAX_THREADS] = { 0, };
char path_prefix[MAX_DIR_NUM][PATH_PREFIX_MAX_LEN];

#define LOCK(a) while(__sync_lock_test_and_set(&a, 1)) {;}
#define UNLOCK(a) __sync_lock_release(&a);

int goToStop = 0;

struct thread_data {
	int idx;
	int logfd;
};

struct gstatistic {
	struct tm cur_tm;
	unsigned long long ticks; /* uptime in seconds        */
	unsigned long long last_ticks;

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
};

struct gstatistic gstats;

enum DIR_SWITCH_SOURCE {
	DAY_DAY_SWITCH = 0, HOUR_HOUR_SWITCH = 1
};

struct {
	int nWorkers; /* number of threads configured  */
	int nr_dir;
	in_addr_t addr; /* local IP or INADDR_ANY   */
	int port; /* local port to listen on  */
	int fd; /* listener descriptor      */

	void *session_array[NR_MAX_THREADS]; /* point to sessions array */
	int curr_nr_conn[NR_MAX_THREADS];

	int signal_fd; /* used to receive signals  */
	int epoll_fd[NR_MAX_THREADS + 1]; /* used for all notification*/
	int verbose;

	int dir_switch_soruce;
	int pid; /* our own pid              */
	char *prog;
} cfg = { .nWorkers = 1, .nr_dir = 1, .dir_switch_soruce = DAY_DAY_SWITCH,
		.addr = INADDR_ANY, /* by default, listen on all local IP's   */
		.fd = -1, .signal_fd = -1, .epoll_fd = { -1, }, };

/* signals that we'll accept synchronously via signalfd */
int sigs[] = { SIGIO, SIGHUP, SIGTERM, SIGINT, SIGQUIT, SIGALRM };

struct session {
	// init val should be NULL
	void *disk_buf;
	// init val should be buf1
	void *cur_buf;

	void *buf1;
	void *buf2;

	int disk_io_ops_lock;
	int disk_io_in_progress;

	int threadidx;
	// len to write
	int disk_write_len;

	// ipv4 only now
	int ip;

	int events;
	int socket_fd;
	int on_disk_fd;
	int membuf_offset;

	int slot_idx;
	int dir_switch_idf;
	int need_delay_free;
	int time_out_need_flush_to_disk;

	unsigned long long last_disk_io_write_tick;
};

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
	char date_str2[16];
	char ip_str1[16];
	char ip_str2[16];
	char *p;

	int ip = s->ip;

	strftime(date_str1, 9, "%Y%m%d", &(gstats.cur_tm));
	strftime(date_str2, 11, "%Y%m%d%H", &(gstats.cur_tm));

	p = (char *) &ip;
	sprintf(ip_str1, "%03u_%03u_%03u_%03u", p[3] & 0xFF, p[2] & 0xFF,
			p[1] & 0xFF, 0);
	sprintf(ip_str2, "%03u_%03u_%03u_%03u", p[3] & 0xFF, p[2] & 0xFF,
			p[1] & 0xFF, p[0] & 0xFF);

	// "/data%2d/log/%s/%s/%s/%s.%s.log"
	sprintf(fileName, DEFAULT_DIR_PREFIX, s->threadidx % cfg.nr_dir + 1,
			date_str1, date_str2, ip_str1, ip_str2, date_str2);

	if (mkpath(fileName, 0755)) {
		fprintf(stderr, "[ERROR] File %s can not setup upper directory: %s\n",
				fileName, strerror(errno));
		return -1;
	}

	fd = open(fileName, O_CREAT | O_WRONLY | O_APPEND);
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

	s->buf1 = calloc(2, DATA_BLOCK_SIZE);
	if (s->buf1 == NULL) {
		free(s);
		return NULL;
	}

	s->buf2 = s->buf1 + DATA_BLOCK_SIZE;
	s->cur_buf = s->buf1;
	s->disk_buf = NULL;
	s->need_delay_free = 0;

	s->events = events;
	s->threadidx = tid;
	s->ip = ip;
	s->socket_fd = socket_fd;

	// must set to -1, as 0 is meaningful
	s->on_disk_fd = -1;
	// s->slot_idx is set in the last step of get session
	s->slot_idx = -1;

	switch (cfg.dir_switch_soruce) {
	default:
		s->dir_switch_idf = gstats.cur_tm.tm_mday;
	case HOUR_HOUR_SWITCH:
		s->dir_switch_idf = gstats.cur_tm.tm_hour;
	}

	if (session_set_on_disk_log_filename(s) < 0) {
		free(s->buf1);
		free(s);
		return NULL;
	}

	return s;
}

/* this rotine does not handle network fd */
inline int free_session_res(struct session* s) {
	close(s->on_disk_fd);
	if(cfg.verbose > 1) {
		fprintf(stderr, "[DEBUG] %s free s=%p buf1=%p dbuf=%p\n", __func__, s, s->buf1, s->disk_buf);
	}
	if (s->buf1) {
		free(s->buf1);
		s->buf1 = NULL;
	}
	return 0;
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
	time_t cur_time;
	struct tm tm_info;

	time(&cur_time);
	localtime_r(&cur_time, &tm_info);

	// update hourly
	if (tm_info.tm_hour != gstats.cur_tm.tm_hour) {
		localtime_r(&cur_time, &(gstats.cur_tm));
	}

	diff_ticks = gstats.ticks - gstats.last_ticks;
	gstats.last_ticks = gstats.ticks;

	busy_poll_loops = gstats.disk_io_threads_busy_loops
			- gstats.last_disk_io_threads_busy_loops;
	gstats.last_disk_io_threads_busy_loops = gstats.disk_io_threads_busy_loops;

	fprintf(stdout, "up %d seconds\n", gstats.ticks);
	fprintf(stdout, "alive    force_wr total_wr succ_fin failed_f polls_ps\n");
	fprintf(stdout, "% 8d % 8d % 8d % 8d % 8d % 8.0f\n\n",
			gstats.snapshot_session_alive,
			gstats.period_succ_submit_disk_io_timer_expire_forced / diff_ticks,
			gstats.period_succ_submit_disk_io / diff_ticks,
			gstats.period_succ_finished_disk_io / diff_ticks,
			gstats.period_failed_finished_disk_io / diff_ticks,
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

		fflush(stdout);
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



// mainly for free struct session, and maybe reset cfg.session_array[tidx][slot] if slot got
inline void delete_session_ptr(struct session *s) {
	int pos, thread_idx;
	struct session **sptr;

	if (!s) {
		return;
	}

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
int del_session(struct session* s) {
	int rc = 0;
	struct epoll_event ev;
	int idx = s->threadidx;
	int fd = s->socket_fd;

	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_DEL, fd, &ev);
	if (rc == -1) {
		fprintf(stderr, "[ERROR] %s epoll_ctl: %s\n", __func__,
				strerror(errno));
	}
	close(fd);

	//TODO FIXME
	// will be free in disk write threads
	if (s->membuf_offset > 0 && (s->slot_idx > -1)) {
		LOCK(s->disk_io_ops_lock);
		// if there is no io thread (and no pending write), update fields, else do nothing
		if (s->disk_buf == NULL) {
			// at this time, lock held, io_in_progress is 1, or io done with io_in_progress cleared
			s->disk_write_len = s->membuf_offset;
			s->disk_buf = s->cur_buf;
			// tell IO thread to clear in-use resource, may be these is case missing
			// so need another threads to do clean up these orphans
			s->need_delay_free = 1;
			UNLOCK(s->disk_io_ops_lock);

			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] ^^^ delay write %d bytes at %p for fd %d on thread %d slot %d\n",
						s->disk_write_len, s->disk_buf, fd, idx, s->slot_idx);
			}
		} else {
			s->need_delay_free = 1;
			// there is pending io, io_in_progress must be 0, yet not started to set s->disk_buf to NULL
			UNLOCK(s->disk_io_ops_lock);

			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] ^^^ discard %d bytes for fd %d on thread %d due to pending IO, io_in_progress %d\n",
						s->disk_write_len, fd, idx, s->disk_io_in_progress);
			}

		}

		// remain will be free and close in io thread
		return 0;
	}

	// s->membuf_offset == 0 || s->slot_idx == -1
	// there is not delay pending action to consume the s->cur_buf, do all the clean up
	free_session_res(s);
	delete_session_ptr(s);
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

	int fd, i, succ = 0;
	struct sockaddr_in in;
	socklen_t sz = sizeof(in);
	int thread_idx = 0;
	int hash = 0;
	struct session *s = NULL, **sptr = NULL;

	fd = accept(cfg.fd, (struct sockaddr*) &in, &sz);
	if (fd == -1) {
		fprintf(stderr, "[ERROR] accept: %s\n", strerror(errno));
		goto done;
	}

	hash = ntohl(in.sin_addr.s_addr) & 0xFF;
	thread_idx = hash % cfg.nWorkers;

	if (cfg.verbose > 0) {
		fprintf(stderr,
				"[INFO] connection fd %d from %s:%d hash %d and assigned to thread %d\n",
				fd, inet_ntoa(in.sin_addr), (int) ntohs(in.sin_port), hash,
				thread_idx);
	}

	//int get_new_session(int ip, int events, int socket_fd, int tid)
	s = get_new_session(ntohl(in.sin_addr.s_addr), EPOLLIN, fd, thread_idx);
	if (s == NULL) {
		close(fd);
		fd = -1;
		goto done;
	}

	// s != NULL
	if ((add_epoll_to_session(s) != -1)) {
		LOCK(conn_slot_lock[thread_idx]);
		i = get_free_conn_slot(thread_idx);
		if (i < 0) {
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
			sptr[i] = s;
			__atomic_fetch_add(&(cfg.curr_nr_conn[thread_idx]), 1, __ATOMIC_SEQ_CST);
			s->slot_idx = i;
			UNLOCK(conn_slot_lock[thread_idx]);
			// memory barrier of unlock
			succ = 1;
		}
	}

	// add_epoll_to_session(s) may be -1 here, succ remains zero
	// get_free_conn_slot may fail here, succ remains zero

	done: if (succ) {
		if (cfg.verbose > 0) {
			fprintf(stderr, "[DEBUG] Added session pointer %p to slot %d, "
					"thread %d, buf1 %p buf2 %p\n", s, i, s->threadidx, s->buf1,
					s->buf2);
		}

		return fd;
	}

	// if fd > 0, s will not null, del_session will close fd
	// delete_session_ptr called in del_session if no delay action is need, and curr_nr_conn[i] non NULL
	if (s) {
		del_session(s);
	}
	return -1;
}

int write2disk(struct session *s) {

	int rc = 0;
	int towrite = 0;
	int ntries = 3;
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

	nleft = s->disk_write_len;
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

	if (!ntries) {
		//warn
	}

	LOCK(s->disk_io_ops_lock);
	s->disk_io_in_progress = 0;
	UNLOCK(s->disk_io_ops_lock);

	return towrite - nleft;
}

int disk_write_thread(void *data) {
	int thread_idx, slot_idx, rc, fd, tid;
	struct session *s = NULL, **sptr;
	int dir_switch_source;

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

				gstats.disk_io_threads_busy_loops++;
				stat_session_alive++;

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
						s->disk_write_len = s->membuf_offset;
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
								s->disk_write_len, s->socket_fd, s->disk_buf,
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
						&& (gstats.ticks - s->last_disk_io_write_tick)
								> FLUSH_DURATION_FORCED
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

					if (cfg.verbose > 2) {
						fprintf(stderr,
								"[INFO] *** S1 force flush to disk due to timeout(%lu %lu) with lenth %d"
										" for fd %d, d %p m %p %p %p\n",
								s->last_disk_io_write_tick, gstats.ticks,
								s->disk_write_len, s->socket_fd, s->disk_buf,
								s->cur_buf, s->buf1, s->buf2);
					}

					// to schedule in the next loop
					continue;
				}

				if (s->disk_buf == NULL) {
					continue;
				}

				tid = s->threadidx;
				fd = s->socket_fd;

				//assert(s->disk_write_len > 0);
				if (s->disk_write_len < 1) {
					fprintf(stderr,
							"[BUG] *** TICK %lu / %lu, thread %d with lenth %d"
									" for fd %d, d %p m %p %p %p; ",
							s->last_disk_io_write_tick, gstats.ticks,
							s->threadidx, s->disk_write_len, s->socket_fd,
							s->disk_buf, s->cur_buf, s->buf1, s->buf2);
					fprintf(stderr,
							"FLAG flush %d io_in_progress %d, IP %08x\n",
							s->time_out_need_flush_to_disk,
							s->disk_io_in_progress, s->ip);
					//TODO DUMP other info
					continue;
				}

				assert(s->threadidx == thread_idx);

				//check if need to write new file
				// day-day directory switch will be tm_mday, hour-hour switch will be tm_hour
				switch (cfg.dir_switch_soruce) {
				default:
					dir_switch_source = gstats.cur_tm.tm_mday;
				case HOUR_HOUR_SWITCH:
					dir_switch_source = gstats.cur_tm.tm_hour;
				}

				if (s->dir_switch_idf != dir_switch_source) {
					// if the get the fd not to -1, set successful switch flag
					if (session_set_on_disk_log_filename(s) != -1) {
						s->dir_switch_idf = dir_switch_source;
					}
					if (cfg.verbose > 1) {
						fprintf(stderr,
								"[INFO] *** Switch DIR result thread %d cfg %d session %d gstats %d\n",
								s->threadidx, cfg.dir_switch_soruce,
								s->dir_switch_idf, dir_switch_source);
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
							tid, fd, s->disk_write_len, rc);
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
				if (s->need_delay_free) {
					if (cfg.verbose > 0) {
						fprintf(stderr,
								"[DEBUG] ^^^ delay free with length %d for fd %d thread %d/%d ip %08x, d %p m %p %p %p\n",
								s->disk_write_len, s->socket_fd, s->threadidx, s->slot_idx, s->ip,
								s->disk_buf, s->cur_buf, s->buf1, s->buf2);
					}
					free_session_res(s);
					delete_session_ptr(s);
				}
			} // end per thread
		} // end all thread

		if (goToStop) {
			break;
		}
	} // should not return

	return 0;
}

/* s->lock maybe hold by */

int pass2write(struct session *s, void *buf, int len) {

	void* dist_buf = NULL;

	if (!s || !buf || len < 1) {
		return -1;
	}

	// there is case timer expire write threads is or will access s->cur_buf and etc
	// there is slot time_out_need_flush_to_disk already 1, but s->cur_buf and etc not updated
	// this is race condition, a write thread is access s->disk_buf
	// all these race condition need be handled

	if (s->membuf_offset + len > DATA_BLOCK_SIZE) {

		LOCK(s->disk_io_ops_lock);

		// check whether s->disk_buf is hold by disk io thread
		// io in submit progress or in progress, s->disk_buf could not be updated
		// disk_buf not NULL, maybe disk_io_in_progress not started yet
		if (s->disk_buf || s->disk_io_in_progress) {
			// message discard
			// warn TODO
			UNLOCK(s->disk_io_ops_lock);
			if (cfg.verbose > 1) {
				fprintf(stderr,
						"[INFO] \t\tthread %d/%d ip %08x discard fd %d with msg_len %d, "
								"mem_buf %p (len %d) dis_buf %p (len %d) and io_in_progress %d\n",
						s->threadidx, s->slot_idx, s->ip, s->socket_fd, len, s->cur_buf,
						s->membuf_offset, s->disk_buf, s->disk_write_len, s->disk_io_in_progress);
			}
			return -1;
		}

		// if s->time_out_need_flush_to_disk == 1;
		// clear the flag, cancel the pending force flush write, as I will fire in this function
		//__sync_val_compare_and_swap (&(s->time_out_need_flush_to_disk), 1, 0);
		if (s->time_out_need_flush_to_disk) {
			s->time_out_need_flush_to_disk = 0;
		}

		// write to disk, check switching to which buffer
		if (s->cur_buf == s->buf1) {
			s->cur_buf = s->buf2;
			s->disk_buf = s->buf1;
		} else {
			s->cur_buf = s->buf1;
			s->disk_buf = s->buf2;
		}
		s->disk_write_len = s->membuf_offset;
		// reset offset to zero
		s->membuf_offset = 0;

		UNLOCK(s->disk_io_ops_lock);

		if (cfg.verbose > 3) {
			fprintf(stderr,
					"[DEBUG] *** Buf Full flush to disk (%lu %lu) with lenth %d"
							" for fd %d, d %p m %p %p %p\n",
					s->last_disk_io_write_tick, gstats.ticks, s->disk_write_len,
					s->socket_fd, s->disk_buf, s->cur_buf, s->buf1, s->buf2);
		}

		// no one is access s->cur_buf and s->membuf_offset, safe update [first copy to the new buf]
		// s->membuf_offset 0 here, but for easy code understaning consistence
		dist_buf = s->cur_buf + s->membuf_offset;
		memcpy(dist_buf, buf, len);
		s->membuf_offset += len;

	} else {
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
	char buf[NET_READ_BUF];

	int fd = s->socket_fd;
	int thread_idx = s->threadidx;

	if (s->slot_idx < 0) {
		// return in case the session setup not compelete
		return;
	}

	rc = read(fd, buf, sizeof(buf));
	switch (rc) {
	default:
		if (cfg.verbose > 3) {
			fprintf(stderr, "[DEBUG] \tthread %d received %d bytes\n",
					thread_idx, rc);
		}
		readOK = 1;
		gstats.net_in_bytes[thread_idx] += rc;
		pass2write(s, (void *) buf, rc);
		break;
	case 0:
		if (cfg.verbose > 0) {
			fprintf(stderr, "[INFO] thread %d fd %d closed\n", thread_idx, fd);
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
	if (cfg.verbose > 0) {
		fprintf(stderr, "[INFO] thread %d client %d has closed\n", thread_idx,
				fd);
	}
	del_session(s);
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
			assert(ev[i].events & EPOLLIN);
			struct session *s = ev[i].data.ptr;

			if (cfg.verbose > 3) {
				fprintf(stderr, "[DEBUG] * thread %d handle POLLIN on fd %d\n",
						thread_idx, s->socket_fd);
			}
			drain_client(s);
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
	time_t cur_time;
	time(&cur_time);
	memset(&gstats, 0, sizeof(gstats));
	localtime_r(&cur_time, &(gstats.cur_tm));

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
	cfg.pid = getpid();

	while ((opt = getopt(argc, argv, "vp:a:n:m:qh")) != -1) {
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
		case 'q':
			cfg.dir_switch_soruce = HOUR_HOUR_SWITCH;
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
					if ((++gstats.ticks % 10) == 0) {
						periodic();
					}
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

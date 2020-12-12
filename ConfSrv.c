// usage ./confUpdateServer -a 192.168.1.1 -n 8 -p 4083 -k 10800 -d 5000

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
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
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>

#define NR_MAX_THREADS 16
#define MAX_CONN_PER_THREAD 409600
#define FLUSH_DURATION_DEFAULT 240
#define FLUSH_DURATION_VARIABLE 60
#define NR_MAX_POLL 8192
#define TEST_IDX 0
#define EXIT_DELAY 1
#define NET_READ_BUF (1 * 64) //  64 Bytes
#define DEFAULT_DATA_BLOCK_SIZE (1 * 256) //  0.25 KiB

#define REALLOC_LAT 180
#define TZ_OFFSET 8

#define BASE_DISCONN_SECS 86400
#define VARI_DISCONN_SECS 86400

#define TOTAL_IPV4S (256 * 256 * 256)
#define INPUT_BUF_SIZE (8 * 1024 * 1024)

#define LOCK(a) while(!__sync_bool_compare_and_swap(&a, 0, 1)) {;}
#define UNLOCK(a) __sync_lock_release(&a);

int conn_slot_lock[NR_MAX_THREADS] = { 0, };
//FIXME concurrent hash map in future?, 16M * 4Bytes
int ip_to_slot_idx[TOTAL_IPV4S];

int goToStop = 0;

char *conf_data_dir_prefix = "/data01/steconf/";


struct gstatistic {
	struct tm cur_tminfo;
	time_t cur_time;
	char date_str[32];
	unsigned long long ticks; /* uptime in seconds        */
	unsigned long long last_report_tick;
	unsigned long long last_dir_set_tick;

	unsigned long long net_out_bytes[NR_MAX_THREADS];
	unsigned long long net_in_bytes[NR_MAX_THREADS];
	unsigned long long last_net_out_bytes[NR_MAX_THREADS];
	unsigned long long last_net_in_bytes[NR_MAX_THREADS];

	unsigned long long sessions_busy_loops;
	unsigned long long last_sessions_busy_loops;

	int snapshot_session_alive;
	int period_succ_submit_disk_io_timer_expire_forced;
	int period_succ_submit_disk_io;
	int period_succ_finished_disk_io;
	int period_failed_finished_disk_io;
	int cur_seq;

	unsigned long long failed_disk_bytes_write;
};

struct gstatistic gstats;

struct {
	int nWorkers; /* number of threads configured  */
	int nr_dir;
	int port; /* local port to listen on  */
	int fd; /* listener descriptor      */

	size_t hash_slot_size;
	in_addr_t addr; /* local IP or INADDR_ANY   */

	void *session_array[NR_MAX_THREADS]; /* point to sessions array */
	int curr_nr_conn[NR_MAX_THREADS];

	int epoll_fd[NR_MAX_THREADS + 1]; /* used for all notification*/
	int signal_fd; /* used to receive signals  */

	int verbose;
	int force_disconnect_seconds;
	int loop_thread_delay_scan_ms;

	int pid; /* our own pid              */
	char *prog;
} cfg = { .nWorkers = 1, .nr_dir = 1, .addr = INADDR_ANY, /* by default, listen on all local IP's   */
.fd = -1, .signal_fd = -1, .epoll_fd = { -1, } };

/* signals that we'll accept synchronously via signalfd */
int sigs[] = { SIGIO, SIGHUP, SIGTERM, SIGINT, SIGQUIT, SIGALRM };

struct session {
	// ipv4 only now
	int ip;
	int to_be_free;

	int events;
	int socket_fd;
	int threadidx;
	int slot_idx;
	int conf_ts;
	int need_reply;
	int nr_reply_bytes;
	struct epoll_event wakeup_epe;

	unsigned long long start_ticks;
	unsigned long long last_alive_tick;
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

struct session* get_new_session(int ip, int events, int socket_fd, int tid) {

	struct session* s = calloc(1, sizeof(struct session));
	if (s == NULL) {
		return NULL;
	}

	s->events = events;
	s->threadidx = tid;
	s->ip = ip;
	s->to_be_free = 0;
	s->socket_fd = socket_fd;

	// s->slot_idx is set in the last step of get session
	s->slot_idx = -1;
	s->start_ticks = gstats.ticks;
	s->last_alive_tick = gstats.ticks;

	s->conf_ts = -1;
	s->need_reply = 0;
	s->nr_reply_bytes = 0;

	// delay to call session_set_on_disk_log_filename, when need to write to disk
	return s;
}

/* mark session in delete, and handle network fd, s will be free asynchronized */
int free_session_step1(struct session* s) {

	if (!s) {
		return -1;
	}

	int rc = 0;
	struct epoll_event ev;
	int sfd = s->socket_fd;
	int ip = s->ip;
	int thread_idx = s->threadidx;

	ip_to_slot_idx[ip & 0x00FFFFFF] = -1;

	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[thread_idx], EPOLL_CTL_DEL, sfd, &ev);
	if (rc == -1) {
		fprintf(stderr, "[ERROR] %s epoll_ctl: %s\n", __func__,
				strerror(errno));
	}
	close(sfd);
	s->socket_fd = -1;

	// free session should be protect as other thread may working on it
	// leave this to be free asynchonize
	// free(s);
	s->to_be_free = 1;

	return rc;
}

int free_session_step2(struct session* s) {

	int rc = 0;
	struct session **sptr = NULL;
	int pos = s->slot_idx;
	int thread_idx = s->threadidx;

	int sfd = s->socket_fd;
	int ip = s->ip;

	// not in this logic
	if (1 != s->to_be_free) {
		return -1;
	}

	s->to_be_free = 2;

	sptr = cfg.session_array[thread_idx];
	// assert sptr[pos] == this session
	if (pos > -1) {
		__atomic_fetch_add(&(cfg.curr_nr_conn[thread_idx]), -1,
				__ATOMIC_SEQ_CST);
		sptr[pos] = NULL;
	}

	if (cfg.verbose > 0) {
		char *p = (char *) &ip;
		fprintf(stderr,
				"[NOTICE] %s connection close fd %d from %d.%d.%d.%d from thread %d slot %d\n",
				gstats.date_str, sfd, p[3] & 0xff, p[2] & 0xff, p[1] & 0xff,
				p[0] & 0xff, thread_idx, pos);
	}

	free(s);
	return 0;
}

// del_session will close network connection
int del_session(struct session* s, int immediately) {
	int rc = free_session_step1(s);
	if (immediately) {
		rc = free_session_step2(s);
	}
	// else leave free action asynchonized
	return rc;
}

void usage() {
	fprintf(stderr, "usage: %s [-v] [-a <ip>] -p <port>\n", cfg.prog);
	exit(-1);
}

/* do periodic work here */
void periodic() {
	int i;
	double net_out_bytes[NR_MAX_THREADS];
	double net_in_bytes[NR_MAX_THREADS];
	unsigned long long diff_ticks = 0;
	unsigned long long busy_poll_loops = 0;

	// must go first, as cur_time as used to cal gstats.cur_seq
	time(&gstats.cur_time);
	localtime_r(&gstats.cur_time, &gstats.cur_tminfo);
	strftime(gstats.date_str, 32, "%Y-%m-%d %H:%M:%S", &gstats.cur_tminfo);

	gstats.ticks++;
	if ((gstats.ticks % 10) != 0) {
		return;
	}

	// print every 10s
	diff_ticks = gstats.ticks - gstats.last_report_tick;
	gstats.last_report_tick = gstats.ticks;

	busy_poll_loops = gstats.sessions_busy_loops
			- gstats.last_sessions_busy_loops;
	gstats.last_sessions_busy_loops = gstats.sessions_busy_loops;

	fprintf(stdout, "%s up_secs %ld failed_disk_write_bytes %ld\n",
			gstats.date_str, gstats.ticks, gstats.failed_disk_bytes_write);
	fprintf(stdout,
			"         time_stamp up_secs  alive    force_wr total_wr succ_fin failed_f polls_ps\n");
	fprintf(stdout, "%s % 8d % 8d % 8.2f % 8.2f % 8.2f % 8.2f % 8.2f\n\n",
			gstats.date_str, gstats.ticks, gstats.snapshot_session_alive,
			(double) gstats.period_succ_submit_disk_io_timer_expire_forced
					/ (double) diff_ticks,
			(double) gstats.period_succ_submit_disk_io / (double) diff_ticks,
			(double) gstats.period_succ_finished_disk_io / (double) diff_ticks,
			(double) gstats.period_failed_finished_disk_io
					/ (double) diff_ticks,
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
			fprintf(stdout, "%.03f (% 3d)\t", gstats.net_out_bytes[i] / 1024.0f,
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
					(double) (gstats.net_out_bytes[i]
							- gstats.last_net_out_bytes[i]) / 1024.0f, i);
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
		net_out_bytes[i] = (double) (gstats.net_out_bytes[i]
				- gstats.last_net_out_bytes[i]);
		gstats.last_net_out_bytes[i] = gstats.net_out_bytes[i];
		fprintf(stdout, "%.03f (% 3d)\t",
				net_out_bytes[i] / 1024.0f / (double) diff_ticks, i);
	}
	fprintf(stdout, "\n\n\n");

	fflush(stdout);
}

/*
 *            typedef union epoll_data {
               void        *ptr;
               int          fd;
               uint32_t     u32;
               uint64_t     u64;
           } epoll_data_t;
 */
int add_epoll_to_session(struct session* s) {
	int rc;
	struct epoll_event *ev = &(s->wakeup_epe);
	int idx = s->threadidx;
	int fd = s->socket_fd;
	int events = s->events;

	memset(ev, 0, sizeof(struct epoll_event));
	ev->events = events;
	ev->data.ptr = s;
	//ev->data.fd = fd;  // union are conflict!!!
	if (cfg.verbose > 1)
		fprintf(stderr, "[INFO] adding fd %d thread idx %d to epoll\n", fd,
				idx);
	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_ADD, fd, ev);
	if (rc == -1) {
		fprintf(stderr, "%s epoll_ctl: %s\n", __func__, strerror(errno));
	}
	return rc;
}

// assert s and s->wakeup_epe
int mod_event_epoll_to_session(struct session* s, int op, int opflag) {
	int rc = 0;
	int idx = s->threadidx;
	int fd = s->socket_fd;
	// must operating on a wake up epoll object
	struct epoll_event *ev = &(s->wakeup_epe);

	if (op == 1) {
		ev->events = s->events | opflag;
	}
	if (op == 0) {
		ev->events = s->events & (~opflag);
	}
	ev->data.ptr = s;

	if (cfg.verbose > 1) {
		fprintf(stderr, "[INFO] mod fd %d thread idx %d to epoll\n", fd, idx);
	}
	/* thread safe for epoll_* api from multiple threads */
	rc = epoll_ctl(cfg.epoll_fd[idx], EPOLL_CTL_MOD, fd, ev);
	if (rc == -1) {
		fprintf(stderr, "%s epoll_ctl: %s\n", __func__, strerror(errno));
	} else {
		s->events = ev->events;
	}
	return rc;
}

int add_epoll(int events, int fd, int idx) {
	int rc;
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
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


int check_ts(int ipv4) {
	char fname[256];
	struct stat fstat;
	int ret = 0;

	int a = (int)((unsigned char *)(&ipv4))[3];
	int b = (int)((unsigned char *)(&ipv4))[2];
	int c = (int)((unsigned char *)(&ipv4))[1];
	int d = (int)((unsigned char *)(&ipv4))[0];
	snprintf(fname, 256, "%s/%03d_%03d_%03d_%03d/%03d_%03d_%03d_%03d.conf",
			conf_data_dir_prefix, a, b, 0, 0, a, b, c, d);

	ret = stat(fname, &fstat);
	if(ret == 0) {
		return fstat.st_ctime;
	} else {
		snprintf(fname, 256, "%s/%03d_%03d_%03d_%03d/%03d_%03d_%03d_%03d.conf",
				conf_data_dir_prefix, a, b, 0, 0, a, b, c, 0);
		ret = stat(fname, &fstat);
		if(ret == 0) {
			return fstat.st_ctime;
		} else {
			return -1;
		}
	}
}


int reply_conf(int ipv4, int sfd, int *fts) {

	int ret = 0;
	int len = 0;
	int total= 0;
	char fname[256];
	char buf[1024];
	int a = (int)((unsigned char *)(&ipv4))[3];
	int b = (int)((unsigned char *)(&ipv4))[2];
	int c = (int)((unsigned char *)(&ipv4))[1];
	int d = (int)((unsigned char *)(&ipv4))[0];

	*fts = -1;
	snprintf(fname, 256, "%s/%03d_%03d_%03d_%03d/%03d_%03d_%03d_%03d.conf",
			conf_data_dir_prefix, a, b, 0, 0, a, b, c, d);
	FILE *fp;
	fp = fopen(fname, "r");
	if(fp == NULL) {
		snprintf(fname, 256, "%s/%03d_%03d_%03d_%03d/%03d_%03d_%03d_%03d.conf",
				conf_data_dir_prefix, a, b, 0, 0, a, b, c, 0);
		fp = fopen(fname, "r");
	}
	if(fp == NULL) {
		// WARN file not exist
		return -1;
	}
	while(fgets(buf, 1024, fp)) {
		buf[1022] = '\0';
		len = strlen(buf);
		buf[len] = '\n';
		// add an extra NULL terminator, but not send to peer, just safe guard
		buf[len + 1] = '\0';
		ret = write(sfd, buf, len);
		// TODO
		if(ret <= 0) {
			fclose(fp);
			return -1;
		} else {
			total += ret;
		}
		continue;
	}

	struct stat fstat;
	ret = stat(fname, &fstat);
	if(ret == 0) {
		*fts = fstat.st_ctime;
	}

	fclose(fp);
	return total;
}


/* accept a new client connection to the listening socket */
int accept_client() {
	int ret = 0;
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

	int ipv4 = ntohl(in.sin_addr.s_addr);
	int fts = 0;

	ret = reply_conf(ipv4, sfd, &fts);
	//sleep(5);
	if(ret < 0 || fts < 0) {
		printf("%d %d\n", ret, fts);
		close(sfd);
		sfd = -1;
		goto done;
	}


	//s = get_new_session(ipv4, EPOLLIN | EPOLLOUT | EPOLLET, sfd, thread_idx);
	s = get_new_session(ipv4, EPOLLIN | EPOLLET, sfd, thread_idx);
	if (s == NULL) {
		close(sfd);
		sfd = -1;
		goto done;
	}
	s->conf_ts = fts;

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
			__atomic_fetch_add(&(cfg.curr_nr_conn[thread_idx]), 1,
					__ATOMIC_SEQ_CST);
			s->slot_idx = slot_idx;
			UNLOCK(conn_slot_lock[thread_idx]);
			// memory barrier of unlock

			ip_to_slot_idx[ipv4 & 0x00FFFFFF] = slot_idx;
			succ = 1;
		}
	}

	// add_epoll_to_session(s) may be -1 here, succ remains zero
	// get_free_conn_slot may fail here, succ remains zero

	done: if (cfg.verbose > 0) {
		fprintf(stderr,
				"[INFO] connection fd %d from %s:%d hash %d and assigned to thread %d, rc=%d\n",
				sfd, inet_ntoa(in.sin_addr), (int) ntohs(in.sin_port), hash,
				thread_idx, succ);

		if (succ) {
			int ip = s->ip;
			char *p = (char *) &ip;
			fprintf(stderr,
					"[NOTICE] %s added %u.%u.%u.%u session pointer %p to slot %d, thread %d\n",
					gstats.date_str, p[3] & 0xff, p[2] & 0xff, p[1] & 0xff,
					p[0] & 0xff, s, slot_idx, s->threadidx);
		}
	}

	if (succ) {
		return sfd;
	}

	// if fd > 0, s will not null
	if (s != NULL) {
		// succ not true, that means s not in session_array]
		free_session_step1(s);
		free(s);
	}
	return -1;
}

// called before write, only the disk write threads
int handle_filesize_exceeded(struct session *s) {
	return 0;
}



void* scan_for_reply_thread(void *data) {
	int thread_idx, slot_idx, rc, sfd, tid;
	struct session *s = NULL, **sptr;

	int stat_session_alive = 0;
	int stat_succ_submit_disk_io_timer_expire_forced = 0;
	int stat_succ_submit_disk_io = 0;
	int stat_succ_finished_disk_io = 0;
	int stat_failed_finished_disk_io = 0;
	int need_ack_input_thread = 0;

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

		// in future, file ts check will be optimze here, but now ...

		// parallel in future, FIXME, TODO
		for (thread_idx = 0; thread_idx < cfg.nWorkers; thread_idx++) {
			sptr = cfg.session_array[thread_idx];

			for (slot_idx = 0; slot_idx < MAX_CONN_PER_THREAD; slot_idx++) {
				s = sptr[slot_idx];
				if (!s) {
					continue;
				}

				// this is DANGER, ASSUME delete is safe, just ASSUME ONLY, may crash the APP
				if (cfg.force_disconnect_seconds > 0
						&& (s->last_alive_tick + cfg.force_disconnect_seconds)
								< gstats.ticks) {
					if (cfg.verbose > 3) {
						fprintf(stderr,
								"[DEBUG] \tthread %d has connection seems dead, last tick: %lld, force disconnected\n",
								thread_idx, s->last_alive_tick);
					}
					// HAS BUG HERE!!!
					del_session(s, 1);
					continue;
				}

				if (1 == s->to_be_free) {
					if (cfg.verbose > 3) {
						fprintf(stderr, "[DEBUG] \tTo free delayed request\n");
					}
					free_session_step2(s);
					continue;
				}

				gstats.sessions_busy_loops++;
				stat_session_alive++;

				// pending need_reply will skip ts checking
				if(s->need_reply == 0) {
					int ts = check_ts(s->ip);
					if (ts > s->conf_ts) {
						fprintf(stderr, "Compare: %d %d\n", ts, s->conf_ts);
						s->need_reply = 1;
						mod_event_epoll_to_session(s, 1, EPOLLOUT);
					}
				}

				if (s->nr_reply_bytes > 0) {
					if (cfg.verbose > 3) {
						fprintf(stderr,
								"[DEBUG] \tthread %d has been reply for ip %x with %d bytes\n",
								thread_idx, s->ip, s->nr_reply_bytes);
					}
					gstats.net_out_bytes[thread_idx] += s->nr_reply_bytes;
					s->nr_reply_bytes = 0;
				}

			} // end per thread
		} // end all thread

		if (goToStop) {
			break;
		}

		if (cfg.loop_thread_delay_scan_ms > 0) {
			usleep(cfg.loop_thread_delay_scan_ms * 1000);
		}
	} // should not return

	return NULL;
}


void drain_client(struct session *s) {
	int rc, readOK = 0;
	int sfd = s->socket_fd;
	int thread_idx = s->threadidx;

	if (s->slot_idx < 0) {
		// return in case the session setup not compelete
		return;
	}

	char buf[1024];
	// WARN maybe not align
	rc = read(sfd, buf, 1024);
	switch (rc) {

	default:
		if (cfg.verbose > 3) {
			fprintf(stderr, "[DEBUG] \tthread %d received %d bytes\n", thread_idx, rc);
			if (buf[rc - 1] == '\n' || buf[rc - 1] == '\0') {
				buf[rc] = '\0';
				fprintf(stderr, "[DEBUG] \tMSG is: %s\n", buf);
			}
		}
		s->last_alive_tick = gstats.ticks;
		readOK = 1;
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
		// ECONNRESET, EINTR
		// !readOK: close the resource
		break;
	}

	if (readOK) {
		return;
	}

	// not safe to force immediatly free
	// this equals to call del_session(s, 0);
	free_session_step1(s);
	// leave other staff to be handle asynchronized
}

// every epoll threads
void* process_thread(void *data) {
	void *dptr;
	int i, n_ready, ret;
	int thread_idx = (int) (unsigned long) data;
	struct epoll_event ev[NR_MAX_POLL];

	/* sleep 1 */

	while (1) {
		n_ready = epoll_wait(cfg.epoll_fd[thread_idx], ev, NR_MAX_POLL, -1);
		if (n_ready < 1) {
			if (errno = -EINTR) {
				continue;
			}
			fprintf(stderr, "[ERROR] thread %d epoll_wait error: %s\n",
					thread_idx, strerror(errno));
			break;
		}

		if (cfg.verbose > 3) {
			fprintf(stderr, "[INFO] got %d epoll ret\n", n_ready);
		}

		for (i = 0; i < n_ready; i++) {
			/* regular POLLIN. handle the particular descriptor that's ready */

			struct session *s = ev[i].data.ptr;
			int sfd = s->socket_fd;

			int ip = s->ip;
			char *p = (char *) &ip;
			if (cfg.verbose > 3) {
				fprintf(stderr,
						"[INFO] %s thread %d handle socket fd %d for event %x"
								" on cli %u.%u.%u.%u %d/%d %x\n", gstats.date_str,
						thread_idx, sfd, ev[i].events, p[3] & 0xff, p[2] & 0xff,
						p[1] & 0xff, p[0] & 0xff, i, n_ready, s->events);
			}

			//assert(ev[i].events & EPOLLIN);
			if (ev[i].events & EPOLLERR || ev[i].events & EPOLLRDHUP) {
				free_session_step1(s);
				// leave other staff to to handle asynchonized
				continue;
			}

			if (ev[i].events & EPOLLIN) {
				// keep on reading
				drain_client(s);
			}

			// send updated conf file if changed, check s->socket_fd, if last step got error
			if (s->socket_fd >= 0 && (ev[i].events & EPOLLOUT)) {
				if(s->need_reply) {
					s->need_reply = 0;
					// this may prevent timeout session free in another thread
					s->last_alive_tick = gstats.ticks;
					int fts = 0;
					ret = reply_conf(s->ip, s->socket_fd, &fts);
					if(ret < 0 || fts < 0) {
						fprintf(stderr, "ERROR!!!\n");
						free_session_step1(s);
					} else {
						// not so accuracy, but simply enough
						if(s->nr_reply_bytes == 0) {
							s->nr_reply_bytes = ret;
						}
						s->conf_ts = fts;
					}
					// end
				}

				// s->socket_fd maybe -1 here if above step has an error
				if(s->socket_fd >= 0) {
					ev[i].events = (~EPOLLOUT) & (s->events);
					ret = epoll_ctl(cfg.epoll_fd[thread_idx], EPOLL_CTL_MOD, s->socket_fd, &(ev[i]));
					if (ret == -1) {
						fprintf(stderr, "[ERROR] %s epoll_ctl: %s\n", __func__,
								strerror(errno));
					} else {
						s->events = ev[i].events;
					}
				}
				// ...
			}


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
	return NULL;
}

void init_gstat(void) {
	memset(&gstats, 0, sizeof(gstats));

	time(&gstats.cur_time);
	localtime_r(&gstats.cur_time, &gstats.cur_tminfo);
	strftime(gstats.date_str, 32, "%Y-%m-%d %H:%M:%S", &gstats.cur_tminfo);
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
	cfg.loop_thread_delay_scan_ms = 10000;
	cfg.force_disconnect_seconds = 10800;

	memset(ip_to_slot_idx, -1, TOTAL_IPV4S * sizeof(int));

	while ((opt = getopt(argc, argv, "vp:a:n:l:d:q:k:h")) != -1) {
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
		case 'd':
			cfg.loop_thread_delay_scan_ms = atoi(optarg);
			break;
		case 'k':
			cfg.force_disconnect_seconds = atoi(optarg);
			fprintf(stderr,
					"*** Use force disconnect is danger in current implementation, may lead program crash.\n");
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if (cfg.addr == INADDR_NONE) {
		usage();
	}

	if (cfg.port < 1 || cfg.port > 65535) {
		usage();
	}

	// less or equal to 0 indicate never disconnect
	if (cfg.force_disconnect_seconds < 0) {
		cfg.force_disconnect_seconds = 0;
	}

	// slot set to -1
	for (n = 0; n < TOTAL_IPV4S; n++) {
		ip_to_slot_idx[n] = -1;
	}

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
		if (NULL == (cfg.session_array[i] = (void *) calloc(MAX_CONN_PER_THREAD,
						sizeof(void *)))) {
			perror("[ERROR] could not alloc conn pointer array memory for thread");
			goto done;
		}

		if (0 > (pthread_create(&tid, NULL, process_thread,
						(void *) (unsigned long) i))) {
			perror("[ERROR] could not create worker %dth thread");
			goto done;
		}

		cfg.curr_nr_conn[i] = 0;
		conn_slot_lock[i] = 0;
		sprintf(pname, "net-worker-%03d", i);
		pthread_setname_np(tid, pname);
	}
	fprintf(stderr, "[INFO] %s start with %d worker threads\n", cfg.prog,
			cfg.nWorkers);

	if (0 > pthread_create(&tid, NULL, scan_for_reply_thread, NULL)) {
		perror("could not create IO thread");
		goto done;
	}
	sprintf(pname, "conf-checker");
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
				fprintf(stderr, "[DEBUG] main thread handle POLLIN on fd %d\n",
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

	// free works, still something left TODO

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

// Usage pe -s "/local_dir/shell_script_file_or_binary_to_transerfer" -n 1024  -l /local_dir/ip.list
// file default to transfer to remote /tmp/
// execute as
// Usage pe -s "/remote_dir/command with_any_args" -n 1024 -x  -l /local_dir/ip.list

// compile gcc -O2 -o pe ParallelExecute.c

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <libgen.h>

#define NR_PARALLEL 40960
#define BUF_SIZE 4096
#define EVT_MAX 1024
#define IP_MAX (1024 * 1024 * 1024)
#define PATH_LEN_MAX 256
#define ARGS_TLEN_MAX 256

struct config {
	char script_path[PATH_LEN_MAX];
	char extra_args[ARGS_TLEN_MAX];
	char *base_name;
	int exec;
	int signal_fd;
	int total_ips;
	int parallel_num;
	int verbose;
	int pos;
	int alive;
	int con_timeout;
};

struct stupidMap {
	int key;
	int val;
};

struct config cfg;
int stop = 0;

int iplist[IP_MAX];
//int pidlist[cfg.parallel_num];
struct stupidMap *pidmap;

int sigs[] = { SIGIO, SIGCHLD, SIGHUP, SIGTERM, SIGINT, SIGQUIT, SIGALRM };


int stupidMap_Add(int key, int val) {
	int i = 0, loop = 0;

	while (loop < cfg.parallel_num) {
		if(pidmap[i].key == 0) {
			pidmap[i].key = key;
			pidmap[i].val = val;
			return i;
		}
		i++;
		loop++;
		// i may not start with 0 in the future
		if(i == cfg.parallel_num) {
			i = 0;
		}
	}

	fprintf(stderr, "[ERROR] no free slot\n");
	return -1;
}



int stupidMap_Remove(int key) {
	int i = 0, loop = 0;
	int val = -1;

	while (loop < cfg.parallel_num) {
		if(pidmap[i].key == key) {
			val = pidmap[i].val;
			pidmap[i].key = 0;
			return val;
		}
		i++;
		loop++;
		// i may not start with 0 in the future
		if(i == cfg.parallel_num) {
			i = 0;
		}
	}

	fprintf(stderr, "[ERROR] Not found\n");
	return val;
}





int add_one(int epollfd, int seq, int exec, const char* arg1,
		const char* arg2) {
	int pipefd[2];
	pipe(pipefd);

	int pid = fork();

	if (pid == 0) {
		unsigned char *ip = (unsigned char *) (iplist + seq);
		char timeout_str[64];
		char buf[64];
		close(pipefd[0]);


		dup2(pipefd[1], STDOUT_FILENO);
		dup2(STDOUT_FILENO, STDERR_FILENO);

        sprintf(timeout_str, "-oConnectTimeout=%d", cfg.con_timeout);

		if (exec == 0) {
			const char* path_src = arg1;
			const char* path_dst = arg2;

			sprintf(buf, "root@%u.%u.%u.%u:%s", ip[3], ip[2], ip[1], ip[0],
					path_dst);
			//printf("Child running: %s\n", buf);
			//fflush(stdout);
			execl("/usr/bin/scp", "/usr/bin/scp", timeout_str,
					"-oPasswordAuthentication=no", "-oStrictHostKeyChecking=no",
					"-pr", path_src, buf, NULL);
		} else {
			char buf[64];
			char cmd_path[128];
			sprintf(buf, "root@%u.%u.%u.%u", ip[3], ip[2], ip[1], ip[0]);
			assert((arg1 != NULL) && (arg2 != NULL));
			sprintf(cmd_path, "%s %s", arg1, arg2);
			execl("/usr/bin/ssh", "/usr/bin/ssh", timeout_str,
					"-oPasswordAuthentication=no", "-oStrictHostKeyChecking=no",
					buf, cmd_path, NULL);
		}
		// should never executed here
	} else {
		close(pipefd[1]);
		struct epoll_event ev;
		ev.data.u64 = ((unsigned long) seq << 32) | pipefd[0];
		ev.events = EPOLLIN;
		epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &ev);
	}
	return pid;
}

int getIPList(const char *fpath) {
	FILE *fp = fopen(fpath, "r");
	char buf[16];
	int i = 0;
	struct in_addr in;

	while (fgets(buf, 16, fp)) {
		buf[strlen(buf) - 1] = '\0';
		inet_aton(buf, &in);
		iplist[i] = ntohl(in.s_addr);
		//printf(" === %08x %s\n", iplist[i], buf);
		i++;
	}

	fclose(fp);
	printf("Got %d IPs.\n", i);
	return i;
}

int core_process(int epollfd) {

	struct epoll_event events[EVT_MAX];
	int signal_fd = cfg.signal_fd;
	int i, nr_exit1 = 0, nr_exit2 = 0;
	char buf[BUF_SIZE];
	struct signalfd_siginfo info;

	while (!stop) {
		int r = epoll_wait(epollfd, events, EVT_MAX, -1);
		int end = 0;
		//printf("r = %d\n", r);
		for (i = 0; i < r; ++i) {
			int t = -1;

			// handle signo first

			/* if a signal was sent to us, read its signalfd_siginfo */
			if (events[i].data.fd == signal_fd) {
				if (read(signal_fd, &info, sizeof(info)) != sizeof(info)) {
					fprintf(stderr,
							"[ERROR] failed to read signal fd buffer\n");
					continue;
				}
				switch (info.ssi_signo) {
				case SIGALRM:
					//alarm(1);
					continue;

				case SIGHUP:
					fprintf(stderr,
							"[INFO] got signal SIGHUP (sig %d), ignore\n",
							info.ssi_signo);
					continue;

				case SIGCHLD:

					// wait for any other not trigger SIGCHLD
					// ref https://stackoverflow.com/questions/8398298/handling-multiple-sigchld
					// "By contrast, if multiple instances of a standard signal are delivered while that signal is currently blocked, then only one instance is queued"
					while (1) {
					    int status, slot = -1, ip;
					    unsigned char *p = (unsigned char *)&ip;
					    pid_t pid = waitpid(-1, &status, WNOHANG);
					    if (pid <= 0) {
					        break;
					    }

						slot = stupidMap_Remove(pid);
						assert(slot >= 0);
						ip = iplist[slot];

					    nr_exit2++;
					    int retcode = -1;
					    if(WIFEXITED(status)) {
					    	retcode = WEXITSTATUS(status);
					    } else if (WIFSIGNALED(status)) {
					    	retcode = WTERMSIG(status);
					    }
					    fprintf(stderr, "[INFO] %d %d: pid %d ip %u.%u.%u.%u returned %d\n", nr_exit2,
					    		slot, pid, p[3], p[2], p[1], p[0], retcode);
					    cfg.alive--;
					}

					if(nr_exit2 == cfg.total_ips) {
						stop = 1;
						exit(0);
					}

					/*
					nr_exit1++;
					//fprintf(stderr,
					//		"[INFO] got signal SIGCHLD (sig %d), ignore\n", info.ssi_signo);
					if(info.ssi_status != 0) {
						fprintf(stderr, "[INFO with ERROR] SIGCHLD %8d: pid %d, exit code %d, "
								"errno %d, utime %d, stime %d\n", nr_exit1, info.ssi_pid, info.ssi_status,
								info.ssi_fd, info.ssi_utime, info.ssi_stime);
					} else {
						fprintf(stderr, "[INFO with SUCC] SIGCHLD %8d: pid %d, exit code %d, "
								"errno %d, utime %d, stime %d\n", nr_exit1, info.ssi_pid, info.ssi_status,
								info.ssi_fd, info.ssi_utime, info.ssi_stime);
					}
					waitpid(info.ssi_pid, NULL, 0);
					cfg.alive--;
					*/

					continue;

				default: /* exit */
					fprintf(stderr, "[ERROR] got signal %d\n", info.ssi_signo);
					stop = 1;
					exit(0);
				}
			}



			int seq = (events[i].data.u64 >> 32);
			int ip = iplist[seq];
			unsigned char *p = (unsigned char *)&ip;


			// handle other events
			if (!(events[i].events & EPOLLIN) || (events[i].events & EPOLLERR)
					|| (events[i].events & EPOLLHUP)) {
				//printf("IP=%08x Seq=%d FD=%d: EVT=%x\n", iplist[seq], seq,
				//		events[i].data.fd, events[i].events);
				end = 1;
			}
			//if (!end) {
				t = read(events[i].data.fd, buf, BUF_SIZE);
			//}
			if (t < 1) {
				end = 1;
			} else if (t < BUF_SIZE){
				buf[t] = '\0';
			}

			if (end) {
				epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
				close(events[i].data.fd);
				if(t < 1) {
					continue;
				}
			}

			// printf buf may has overflow issue when t == BUF_SIZE
			printf("IP=%u.%u.%u.%u: Seq=%d, STRLEN=%d, STR=%s\n",
					p[3], p[2], p[1], p[0], seq, t, buf);
		}
		//sleep(1);

		if(stop) {
			break;
		}

		while((cfg.pos < cfg.total_ips) && (cfg.alive < cfg.parallel_num)) {
			//printf("%x\n", iplist[n]);
			int pid;
			pid = add_one(epollfd, cfg.pos, cfg.exec, cfg.script_path, cfg.extra_args);
			stupidMap_Add(pid, cfg.pos);
			cfg.pos++;
			cfg.alive++;
		}
	}

}

int main(int argc, char *argv[]) {

	int epollfd = epoll_create(1);
	struct epoll_event ev;
	char ipListPath[PATH_LEN_MAX] = {'\0',};
	char scriptPath[PATH_LEN_MAX] = {'\0',};

	int n, opt;

	memset(&cfg, 0, sizeof(cfg));

	/* block all signals. we take signals synchronously via signalfd */
	sigset_t all;
	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, NULL);

	/* a few signals we'll accept via our signalfd */
	sigset_t sw;
	sigemptyset(&sw);
	for (n = 0; n < sizeof(sigs) / sizeof(*sigs); n++) {
		sigaddset(&sw, sigs[n]);
	}
	//cfg.signal_fd = signalfd(-1, &sw, SFD_CLOEXEC);
	cfg.signal_fd = signalfd(-1, &sw, 0);

	ev.data.fd = cfg.signal_fd;
	ev.events = EPOLLIN;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, cfg.signal_fd, &ev);

	cfg.parallel_num = NR_PARALLEL;
	cfg.con_timeout = 1;

	while ((opt = getopt(argc, argv, "vxs:e:l:n:t:h")) != -1) {
		switch (opt) {
		case 'v':
			cfg.verbose++;
			break;
		case 'x':
			cfg.exec = 1;
			break;
		case 's':
			strncpy(cfg.script_path, optarg, PATH_LEN_MAX - 1);
			break;
		case 'e':
			strncpy(cfg.extra_args, optarg, ARGS_TLEN_MAX -1);
			break;
		case 'l':
			strncpy(ipListPath, optarg, PATH_LEN_MAX - 1);
			break;
		case 'n':
			cfg.parallel_num = atoi(optarg);
			break;
		case 't':
			cfg.con_timeout = atoi(optarg);
			break;
		case 'h':
		default:
			//usage();
			break;
		}
	}

    if(cfg.con_timeout < 1) {
        cfg.con_timeout = 1;
    }

    if(cfg.con_timeout > 60) {
        cfg.con_timeout = 60;
    }

	if(cfg.parallel_num < 1 || cfg.parallel_num > NR_PARALLEL) {
		cfg.parallel_num = NR_PARALLEL;
	}

	pidmap = malloc(cfg.parallel_num * sizeof(struct stupidMap));
	memset(pidmap, 0, cfg.parallel_num * sizeof(struct stupidMap));


	if(ipListPath[0]) {
		n = getIPList(ipListPath);
	} else {
		n = getIPList("/Users/shine/Documents/GmetadList.txtXXX");
	}

	if(n < 1) {
		printf("No IP list specified, or IP list file invalid\n");
		exit(-1);
	}

	cfg.total_ips = n;

	if(!cfg.script_path[0]) {
		printf("No script or cmd specified\n");
		exit(-1);
	}

	strncpy(scriptPath, cfg.script_path, PATH_LEN_MAX - 1);
	cfg.base_name = basename(scriptPath);

	if(cfg.exec == 0) {
		// scp mode
		sprintf(cfg.extra_args, "/tmp/%s", cfg.base_name);
	} else {
		// ssh execute mode
		// cfg.script_path must start with /tmp
		;
	}


	for(n = 0; n < cfg.total_ips && n < cfg.parallel_num; n++) {
		//printf("%x\n", iplist[n]);
		int pid = add_one(epollfd, n, cfg.exec, cfg.script_path, cfg.extra_args);
		pidmap[n].key = pid;
		pidmap[n].val = n;
	}

	cfg.pos = n;
	cfg.alive = n;

	core_process(epollfd);

	return 0;
}

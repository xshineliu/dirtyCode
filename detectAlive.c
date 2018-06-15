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

#include <linux/tcp.h>

#define IPSTART 12
#define IPEND 254

#define MAX_THREADS 32
#define MAX_FDS_PER_T 10240
#define MAX_CONN_ONE 4096

#define MAX_IP_ADDR 0x0a270000
#define PORT 8649

int ppfds[MAX_THREADS][MAX_FDS_PER_T];



int do_connect(int epollfd, int *ipv4addr) {

	int fdFlag = 0;
	int fd = -1;
	int ret = -1;
	struct epoll_event ev;

	struct timeval timeout;
	struct sockaddr_in addr;
	int opts;

	if((*ipv4addr & 0xFF) == 0x0F) {
		*ipv4addr += 253;
	}

	if((*ipv4addr & 0xFF00) == 0xFF00) {
		*ipv4addr += 0x100;
	}


	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		printf("connect error:%s\n", strerror(errno));
		return -1;
	}

	// LINUX ONLY?
	timeout.tv_sec = 0;
	timeout.tv_usec = 20000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	int sc = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_SYNCNT, &sc, sizeof(sc));

	if ((fdFlag = fcntl(fd, F_GETFL, 0)) < 0) {
		printf("F_GETFL error, %s\n", strerror(errno));
	}

	fdFlag |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, fdFlag) < 0) {
		printf("F_SETFL error, %s\n", strerror(errno));
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(*ipv4addr);
	addr.sin_port = htons(PORT);

	ret = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
	if (ret == -1) {
		if (errno == EINPROGRESS) {
			//printf("TID %04lu connect in progress: %s\n", idx, strerror(errno));
			//continue;
		} else {
			printf("connect %x error: %s\n", ipv4addr, strerror(errno));
			return -1;
		}
	}

	ev.events = EPOLLIN|EPOLLOUT;
	ev.data.u64 = (fd | ((uint64_t) *ipv4addr << 32));
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		printf("epll_ctl: server_sockfd register failed, %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int worker(void *data) {

	int idx = 0;
	int i;
	int ipv4addr = 0x0a03000C;

	int nfds;
	int total_n = 30000;
	int nconn = 0;
	int nclose = 0;
	int epollfd;

	struct epoll_event events[MAX_CONN_ONE];

	int timeout_ms = 10;

	epollfd = epoll_create(MAX_CONN_ONE);
	if (epollfd == -1) {
		printf("epoll_create failed\n");
		return -1;
	}

	for (i = 0; i < MAX_CONN_ONE; i++) {
		if(nconn > total_n) {
			break;
		}
		if(do_connect(epollfd, &ipv4addr) == 0) {
			nconn++;
		}
		ipv4addr++;
	}

	while(1) {

		if (nclose == nconn) {
			if(ipv4addr > MAX_IP_ADDR || nconn >= total_n) {
				break;
			}
		}

		nfds = epoll_wait(epollfd, events, MAX_CONN_ONE, timeout_ms);
		if (nfds == -1) {
			printf("start epoll_wait failed\n");
			return -1;
		}


		for (i = 0; i < nfds; i++) {
			int ip = (events[i].data.u64 >> 32);
			int fd = events[i].data.fd;
			unsigned char *p = (unsigned char *)&ip;

			int ev = events[i].events;
			//if(!(ev & EPOLLERR) && (ev & EPOLLIN)) {
			if(!(ev & EPOLLERR)) {
				printf("Data %08X %u.%u.%u.%u %X\n", ip, p[3], p[2], p[1], p[0], events[i].events);
			}

			epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
			close(fd);
			nclose++;
		}

		if(nfds > 0) {
			printf("Got N=%d CONN %d CLOSE %d\n", nfds, nconn, nclose);
		}


		while (MAX_CONN_ONE + nclose - nconn > 0) {
			if(nconn >= total_n || ipv4addr > MAX_IP_ADDR) {
				break;
			}
			if(do_connect(epollfd, &ipv4addr) == 0) {
				nconn++;
			}
			ipv4addr++;
		}
	}

}


int main(int argc, char* argv[]) {
	worker(NULL);
}

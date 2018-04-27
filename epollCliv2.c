#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
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

#define THREAD_NUM   10
#define PER_THREAD_MAX_SOCKET_FD 20
#define MAX_EVENTS 32768

#define PORT 80

static int processClient();

static int stop = 0;

int main(int argc, char *argv[]) {
	int i = 0;
	int duration = 10;
	pthread_t tid[THREAD_NUM];

	for (i = 0; i < THREAD_NUM; ++i) {
		pthread_create(&tid[i], NULL, (void*) &processClient, (void *)&i);
	}

	if(argc > 1){
		duration = atoi(argv[1]);
		if(duration < 1) {
			duration = 10;
		}
	}

	fprintf(stderr, "%s will run %d seconds", argv[0], duration);
	sleep(duration);
	stop = 1;


	for (i = 0; i < THREAD_NUM; ++i) {
		pthread_join(tid[i], NULL);
		fprintf(stderr, "%s thread %d joined\n", argv[0], i);
	}

	fprintf(stderr, "%s exit\n", argv[0]);
	return 0;
}


int reconnect(int epollFd, void* optval, socklen_t optlen, struct sockaddr* addr, socklen_t sa_len) {

	struct epoll_event ev;
	int fd = -1;
	int fdFlag = 0;
	int ret;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		printf("error get socket fd: %s\n", strerror(errno));
		return fd;
	}
	setsockopt(fd, SOL_SOCKET, 0, optval, optlen);
	if ((fdFlag = fcntl(fd, F_GETFL, 0)) < 0) {
		printf("F_GETFL error");
	}
	fdFlag |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, fdFlag) < 0) {
		printf("F_SETFL error");
	}

	ret = connect(fd, addr, sa_len);
	if (ret == -1) {
		if (errno == EINPROGRESS) {
			//
		} else {
			printf("connect error: %s\n", strerror(errno));
			return fd;
		}
	}

	ev.events = EPOLLOUT;
	ev.data.fd = fd;
	if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		printf("epll_ctl: server_sockfd register failed");
		return -2;
	}

	return fd;
}

int processClient(void *d) {

	int idx = *(int *)d;
	pthread_t tid = pthread_self();

	int fd[PER_THREAD_MAX_SOCKET_FD] = { 0 };
	int ret;
	struct sockaddr_in addr = { 0 };
	struct in_addr srvIP;

	int set = 30;
	int i = 0;
	int fdFlag = 0;

	unsigned long long loops = 0;
	int nr_conn = 0;

	struct epoll_event ev;
	struct epoll_event events[MAX_EVENTS];

	int nfds;

	char sendbuf[512] = { 0 };
	char recvbuf[65536] = { 0 };
	char socketId[10] = { 0 };


	unsigned long long nevt = 0;
	unsigned long long nevt_send = 0;
	unsigned long long nevt_receive = 0;
	unsigned long long nr_close = 0;


	printf("Thread %04lu running.\n", idx);
	inet_aton("10.10.23.58", &srvIP);
	addr.sin_family = AF_INET;
	addr.sin_addr = srvIP;
	addr.sin_port = htons(PORT);


	int epollFd = epoll_create(MAX_EVENTS);
	if (epollFd == -1) {
		printf("epoll_create failed\n");
		return epollFd;
	}


	for (i; i < PER_THREAD_MAX_SOCKET_FD; ++i) {
		fd[i] = socket(AF_INET, SOCK_STREAM, 0);
		if (fd[i] == -1) {
			printf("error:%s\n", strerror(errno));
			return fd[i];
		}

		// set timer is valid ?
		//setsockopt(fd[i], SOL_SOCKET, SO_KEEPALIVE, &set, sizeof(set));
		setsockopt(fd[i], SOL_SOCKET, 0, &set, sizeof(set));
		// set socket non block?
		if ((fdFlag = fcntl(fd[i], F_GETFL, 0)) < 0) {
			printf("F_GETFL error");
		}
		fdFlag |= O_NONBLOCK;
		if (fcntl(fd[i], F_SETFL, fdFlag) < 0) {
			printf("F_SETFL error");
		}

		ret = connect(fd[i], (struct sockaddr*) &addr, sizeof(addr));
		if (ret == -1) {
			if (errno == EINPROGRESS) {
				printf("TID %04lu connect in progress: %s\n", idx, strerror(errno));
				continue;
			} else {
				printf("TID %04lu connect error: %s\n", idx, strerror(errno));
				return fd[i];
			}
		}
		nr_conn++;
	}

	for (i = 0; i < PER_THREAD_MAX_SOCKET_FD; ++i) {
		ev.events = EPOLLOUT;
		ev.data.fd = fd[i];
		if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd[i], &ev) == -1) {
			printf("epll_ctl: server_sockfd register failed");
			return -1;
		}
	}

	printf("TID %04lu enter first part\n", idx);




	for ( ; ; loops++) {

		nfds = epoll_wait(epollFd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			printf("start epoll_wait failed");
			return -1;
		}
		nevt += (unsigned int)nfds;
		printf("Loops %06llu TID %04lu active event number is %d (%ld %ld/%ld/%ld)\n",
				loops, idx, nfds, nevt, nevt_send, nevt_receive, nr_close);
		for (i = 0; i < nfds; i++) {
			/*
			 if ((events[i].events & EPOLLERR) ||
			 (events[i].events & EPOLLHUP) ||
			 (!(events[i].events & EPOLLIN)) ||
			 (!(events[i].events & EPOLLOUT)) )
			 {
			 printf("enter 1");
			 fprintf (stderr, "epoll error\n");
			 close (events[i].data.fd);
			 continue;
			 }
			 */
			if (events[i].events & EPOLLOUT) {

				memset(sendbuf, 0, sizeof(sendbuf));
				sprintf(sendbuf, "GET /2.html HTTP/1.1\r\n Connection: Close\r\n\r\n");
				ret = send(events[i].data.fd, sendbuf, strlen(sendbuf), 0);
				if (ret == -1) {
					if (errno != EAGAIN) {
						printf("TID %04u send error: %s\n", idx, strerror(errno));
						close(events[i].data.fd);
					}
					continue;
				}
				nevt_send++;
				printf(" ===> TID %04lu send %d bytes (x %ld).\n",
						idx, strlen(sendbuf), nevt_send);
				// add revelant socket read event
				ev.data.fd = events[i].data.fd;
				ev.events = EPOLLIN | EPOLLET;
				epoll_ctl(epollFd, EPOLL_CTL_MOD, events[i].data.fd, &ev);

			} else if (events[i].events & EPOLLIN) {

				int count = 0;
				memset(recvbuf, 0, sizeof(recvbuf));
				count = recv(events[i].data.fd, recvbuf, sizeof(recvbuf), 0);
				if (count == -1) {
					/* If errno == EAGAIN, that means we have read all
					 data. So go back to the main loop. */
					if (errno != EAGAIN) {
						printf("TID %04lu read error\n", idx);
						close(events[i].data.fd);
					}
					continue;
				} else if (count == 0) {
					/* End of file. The remote has closed the
					 connection. */

					close(events[i].data.fd);
					nr_close++;
					printf(" * ===> TID %04lu closed total %d\n", idx, nr_close);

					//reconnect
					events[i].data.fd = reconnect(epollFd, &set, sizeof(set),
							(struct sockaddr*) &addr, sizeof(addr));
					if(events[i].data.fd < 0) {
						printf(" * ===> TID %04lu reconnect failed\n", idx);
						nr_conn--;
					}
					continue;
				}
				nevt_receive++;
				printf(" + ===> TID %04lu receive data is: %d %d (x %ld)\n",
						idx, strlen(recvbuf), count, nevt_receive);


				ev.data.fd = events[i].data.fd;
				/* wait for server to close, ACK with FIN */
				ev.events = EPOLLIN | EPOLLET;
				epoll_ctl(epollFd, EPOLL_CTL_MOD, events[i].data.fd, &ev);

				/* if wait for server to read data, eg: only ACK
				ev.data.fd = events[i].data.fd;
				ev.events = EPOLLOUT;
				epoll_ctl(epollFd, EPOLL_CTL_MOD, events[i].data.fd, &ev);
				*/

			} else {
				printf(" ? ===> TID %04d event %d not handled\n", idx, events[i].events);
			}
		} // end for


		if(stop == 1) {
			// close all handles
			break;
		}

	} // while

	//double close?
	for (i = 0; i < PER_THREAD_MAX_SOCKET_FD; ++i) {
		close(fd[i]);
	}


	printf(" ^ ===> TID %04d exit total loop %d (%ld %ld/%ld/%ld)\n",
			idx, loops, nevt, nevt_send, nevt_receive, nr_close);
	pthread_exit(0);
	return 0;
}


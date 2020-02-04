#define _GNU_SOURCE
#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdlib.h>
//#include <locale.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>


#define MAX_LINE_LEN 4096
#define BUFSIZE (2 * 1024 * 1024)
#define NR_MAX_THREADS 16

#define UDP_SERVER_IP "192.168.3.3"
#define UDP_SERVER_PORT 5555

//#define MAX_BACK_SEC (7 * 86400)
#define MAX_BACK_SEC (15 * 60)


time_t udp_conn_last_try_tick[NR_MAX_THREADS] = {0,};
int udp_conn_sfd[NR_MAX_THREADS] = {0,};
struct sockaddr_in udp_srv_sock[NR_MAX_THREADS];
char text_ip[NR_MAX_THREADS][128];
unsigned short remote_port[NR_MAX_THREADS];

static char partial_line[MAX_LINE_LEN];
static char content[BUFSIZE];

static time_t cur_time = 0;

/* addr str len 128 */
int lookup_host(const char *host, char *text_ip) {
	struct addrinfo hints, *res;
	int errcode;
	void *ptr;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags |= AI_CANONNAME;
	errcode = getaddrinfo(host, NULL, &hints, &res);
	if (errcode != 0) {
		perror("getaddrinfo");
		return -1;
	}
	while (res) {
		inet_ntop(res->ai_family, res->ai_addr->sa_data, text_ip, 100);
		switch (res->ai_family) {
		case AF_INET:
			ptr = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
			break;
		case AF_INET6:
			ptr = &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;
			break;
		}
		inet_ntop(res->ai_family, ptr, text_ip, 128);
		//printf("IPv%d address: %s (%s)\n", res->ai_family == PF_INET6 ? 6 : 4,
		//		text_ip, res->ai_canonname);
		res = res->ai_next;
	}
	return 0;
}


int connect_udp_peer(int thread_idx) {
	struct sockaddr_in *udp_sock = NULL;
	int sockfd = 0;
	time_t last_try_tick = 0;
	time_t this_try_tick = 0;
	int ret = 0;

	// in case fd still open
	if(udp_conn_sfd[thread_idx] > 0) {
		close(udp_conn_sfd[thread_idx]);
	}

	// check if the peace period expire
	last_try_tick = udp_conn_last_try_tick[thread_idx];
	time(&this_try_tick);
	//if(this_try_tick < last_try_tick + 10) {
	//	return 1;
	//}
	udp_conn_last_try_tick[thread_idx] = this_try_tick;


	udp_sock = udp_srv_sock + thread_idx;
	memset(udp_sock, 0, sizeof(struct sockaddr_in));

    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
        perror("socket creation failed");
        udp_conn_sfd[thread_idx] = -1;
        //exit(EXIT_FAILURE);
        return 1;
    }
    udp_conn_sfd[thread_idx] = sockfd;

    udp_sock->sin_family = AF_INET;
    udp_sock->sin_port = htons(remote_port[thread_idx]);
    ret = inet_pton(AF_INET, text_ip[thread_idx], &(udp_sock->sin_addr));
	//fprintf(stderr, "inet_pton ret code %d\n", ret);

    return 0;
}

int send_to_udp_peer(int thread_idx, const char *msg, int len) {
	struct sockaddr_in *udp_sock = NULL;
	int sockfd = udp_conn_sfd[thread_idx];
	int ret;

	if( sockfd < 1 ) {
		if(connect_udp_peer(thread_idx) != 0 ) {
			// connnect not succesful
			//fprintf(stderr, "CONN FAIL\n");
			return 1;
		}
	}

	// at this point, valid
	udp_sock = udp_srv_sock + thread_idx;
	sockfd = udp_conn_sfd[thread_idx];

	ret = sendto(sockfd, msg, len, 0,
			(const struct sockaddr *) udp_sock, sizeof(struct sockaddr_in));
	return 0;
}

int get_utc(char *head) {
	char *p = head;
	char *b = head;
	while (*p && *p != '\n' && *p != '|') {
		p++;
	}

	if(*p != '|') {
		return -1;
	}
	b = p;

	p++;
	while (*p && *p != '\n' && *p != '|') {
		p++;
	}

	if(*p != '|') {
		return -1;
	}

	p++;
	while (*p && *p != '\n' && *p != '|') {
		p++;
	}

	if(*p != '|') {
		return -1;
	}
	*p = '\0';

	struct tm result;
    //printf("head:  %s\n", b);
	if (strptime(b, "| %m/%d/%Y | %H:%M:%S", &result) == NULL) {
		return -1;
	}

	int ret_val = (int) mktime(&result);
    //printf("UTC:  %d\n", ret_val);
	return ret_val;
}

int split_and_send(int n_channel, char *msg, int len) {
	int i = 0;
	int n = 0;
	int single_msg_len = 0;

	//printf("Debug len=%d ip=%d\n", len, msg[8]);
	if(len < 10 || msg[8] != ' ') {
		//printf("Debug1 Bad Msg with len %d: %s\n", len, msg);
		return 0;
	}

	msg[8] = '\0';
	int ip = 0;
	ip = strtol(msg, NULL, 16);
	//printf("Debug %d %d %x\n", len, msg[8], ip);

	if(ip == 0) {
		return 0;
	}

	char *ipb = (char *) & ip;
	char *p1 = msg + 9;
	char *p2 = p1;

	char buf[MAX_LINE_LEN];
	char head[64];
	// when p2 - msg == len, *p exceeded already
	while((p2 - msg) < len) {
		if(*p2 != '\n') {
			p2++;
			continue;
		}
		// *p2 == '\n'
		*p2 = '\0';

		int skip = 0;
		snprintf(head, 63, "%s%s", partial_line, p1);
		int utc = get_utc(head);
		//printf("UTC get: %d\n", utc);
		if(utc > 1) {
			int delta = (int) (cur_time - utc);
			if(delta > MAX_BACK_SEC || delta < -MAX_BACK_SEC) {
				skip = 1;
			}
			// allow ipmi time at UTC
			if(skip == 1 && (delta - 3600 * 8 < MAX_BACK_SEC && delta - 3600 * 8 > -MAX_BACK_SEC)) {
				skip = 0;
			}

			if(skip == 1) {
				p2++;
				p1 = p2;
				//printf("UTC skip: %s\n", head);
				continue;
			}
		} else {
			// skip these ill lines. include those started with "Pre-Init"
			skip = 1;
			p2++;
			p1 = p2;
			continue;
		}


		single_msg_len = snprintf(buf, MAX_LINE_LEN - 1, "%d.%d.%d.%d | %s%s\n", ipb[3] & 0xFF, ipb[2] & 0xFF, ipb[1] & 0xFF, ipb[0] & 0xFF, partial_line, p1);

		if(partial_line[0]) {
			partial_line[0] = '\0';
		}

		// Microcontroller BMC Time
		if(!skip && strstr(buf, " | Microcontroller BMC Time Hopping | ") != NULL) {
			skip = 1;
		}
		//if(!skip && strstr(buf, " | Microcontroller #0x") != NULL) {
		//	skip = 1;
		//}
		if(!skip && strstr(buf, " | Event Logging Disabled") != NULL) {
			skip = 1;
		}
		if(!skip && strstr(buf, " | Timestamp Clock Sync") != NULL) {
			skip = 1;
		}
		if(!skip && strstr(buf, " | Session Audit ") != NULL) {
			skip = 1;
		}
		if(!skip && strstr(buf, " | Lower Non-critical going low ") != NULL) {
			skip = 1;
		}


// Lower Non-critical going low | Asserted

		if(skip == 1) {
			//printf("Filter skip: %s\n", buf);
			// skip
			p2++;
			p1 = p2;
			continue;
		}

		//printf("UTC get: %d\n", utc);
		for ( i = 0; i < n_channel; i++) {
			send_to_udp_peer(i, buf, single_msg_len);
		}

		n++;
		p2++;
		p1 = p2;
	}

	if(msg[len - 1] != '\0') {
		// '\0' is added by the caller
		//printf("Left: %s\n", p1);
		strncpy(partial_line, p1, MAX_LINE_LEN - 1);
	} else {
		partial_line[0] = '\0';
	}

	// skip any incomplete msg without '\n'

	return n;
}

/*
 * error - wrapper for perror
 */
void error(char *msg)
{
	perror(msg);
	//exit(1);
}

void* status_report_worker(void * data) {
	while (1) {
		time(& cur_time);
		sleep (10);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int sockfd;		/* socket */
	int portno;		/* port to listen on */
	int clientlen;		/* byte size of client's address */
	struct sockaddr_in serveraddr;	/* server's addr */
	struct sockaddr_in clientaddr;	/* client addr */
	struct hostent *hostp;	/* client host info */
	//char *buf;		/* message buf */
	char *hostaddrp;	/* dotted decimal host addr string */
	int optval;		/* flag value for setsockopt */
	int n;			/* message byte size */
	int i = 0;
	int n_end = 0;
	int ret = 0;

	/*
	 * check command line arguments
	 */
	if (argc < 4 && (argc % 2 != 0)) {
		fprintf(stderr, "usage: %s <port> <remote name> <remote port> [<remote name> <remote port>] \n", argv[0]);
		exit(1);
	}

	portno = atoi(argv[1]);
	n = 2;
	while( n < argc ) {
		lookup_host(argv[n], text_ip[n_end]);
		remote_port[n_end] = atoi(argv[n + 1]);
		printf("Remote %2d: %s %d Added\n", n_end, text_ip[n_end], remote_port[n_end]);
		n += 2;
		n_end++;
	}


	pthread_t tid_status_report_worker;
	if ( 0 > (pthread_create(&tid_status_report_worker, NULL, status_report_worker, NULL)) ) {
		error("ERROR create threads");
		exit(20);
	}
	// sleep (1);

	/*
	 * socket: create the parent socket
	 */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		error("ERROR opening socket");
		exit(2);
	}

	/* setsockopt: Handy debugging trick that lets
	 * us rerun the server immediately after we kill it;
	 * otherwise we have to wait about 20 secs.
	 * Eliminates "ERROR on binding: Address already in use" error.
	 */
	optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
		   (const void *)&optval, sizeof(int));

	/*
	 * build the server's Internet address
	 */
	bzero((char *)&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	//serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	serveraddr.sin_port = htons((unsigned short)portno);

	/*
	 * bind: associate the parent socket with a port
	 */
	if (bind(sockfd, (struct sockaddr *)&serveraddr,
		 sizeof(serveraddr)) < 0) {
		error("ERROR on binding");
		exit(3);
	}

	/*
	 * main loop: wait for a datagram, then echo it
	 */
	clientlen = sizeof(clientaddr);
	while (1) {

		/*
		 * recvfrom: receive a UDP datagram from a client
		 */
		char *buf = content; // gloable storage
		n = recvfrom(sockfd, buf, BUFSIZE - 1, 0,
			     (struct sockaddr *)&clientaddr, &clientlen);
		if (n < 0)
			error("ERROR in recvfrom");

		/*
		 * gethostbyaddr: determine who sent the datagram
		hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr,
				      sizeof(clientaddr.sin_addr.s_addr),
				      AF_INET);
		if (hostp == NULL)
			error("ERROR on gethostbyaddr");
		hostaddrp = inet_ntoa(clientaddr.sin_addr);
		if (hostaddrp == NULL)
			error("ERROR on inet_ntoa\n");
		 */
		buf[n] = '\0';
		//printf("Server received %d bytes: %s\n", n, buf);
		//printf("server received %d bytes\n", n);

		split_and_send(n_end, buf, n);

		/*
		 * sendto: echo the input back to the client
		n = sendto(sockfd, buf, n, 0,
			   (struct sockaddr *)&clientaddr, clientlen);
		if (n < 0)
			error("ERROR in sendto");
		*/
	}
}

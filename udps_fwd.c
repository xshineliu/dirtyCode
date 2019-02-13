#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>


#define MAX_LINE_LEN 4096
#define BUFSIZE 8192
#define NR_MAX_THREADS 16

#define UDP_SERVER_IP "10.10.1.22"
#define UDP_SERVER_PORT 5555

time_t udp_conn_last_try_tick[NR_MAX_THREADS] = {0,};
int udp_conn_sfd[NR_MAX_THREADS] = {0,};
struct sockaddr_in udp_srv_sock[NR_MAX_THREADS];
char text_ip[NR_MAX_THREADS][128];
unsigned short remote_port[NR_MAX_THREADS];

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

int split_and_send(int n_channel, char *msg, int len) {
	int i = 0;
	int n = 0;
	int single_msg_len = 0;

	//printf("Debug %d %d\n", len, msg[8]);
	if(len < 10 || msg[8] != ' ') {
		printf("Debug1 Bad Msg with len %d: %s\n", len, msg);
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
	// when p2 - msg == len, *p exceeded already
	while((p2 - msg) < len) {
		if(*p2 != '\n') {
			p2++;
			continue;
		}
		// *p2 == '\n'
		*p2 = '\0';

		// at least 20 char with xxxx-xx-xx HH:mm:ss\n
		if(p2 - p1 < 20) {
			p2++;
			p1 = p2;
			continue;
		}

		if(p1[4] != '-' || p1[7] != '-') {
			p2++;
			p1 = p2;
			continue;
		}

		single_msg_len = snprintf(buf, MAX_LINE_LEN - 1, "%d.%d.%d.%d %s\n", ipb[3] & 0xFF, ipb[2] & 0xFF, ipb[1] & 0xFF, ipb[0] & 0xFF, p1);
		//printf("Prepare to Send %s", buf);
		for ( i = 0; i < n_channel; i++) {
			send_to_udp_peer(i, buf, single_msg_len);
		}

		n++;
		p2++;
		p1 = p2;
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

int main(int argc, char **argv)
{
	int sockfd;		/* socket */
	int portno;		/* port to listen on */
	int clientlen;		/* byte size of client's address */
	struct sockaddr_in serveraddr;	/* server's addr */
	struct sockaddr_in clientaddr;	/* client addr */
	struct hostent *hostp;	/* client host info */
	char *buf;		/* message buf */
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
		char buf[BUFSIZE] = {0,};
		n = recvfrom(sockfd, buf, BUFSIZE, 0,
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
		//printf("server received %d bytes from %s\n", n, buf); 
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

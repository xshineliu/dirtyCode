// gcc -g  -o sbping sbping.c -lpthread

/*
 * Code based on MBB Ping as:
 *
 * Copyright 2016 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of MBB ping. MBB ping is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * MBB ping is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * MBB ping. If not, see http://www.gnu.org/licenses/.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/filter.h>
#include <linux/icmp.h>
#include <linux/icmpv6.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#define _GNU_SOURCE
#include <pthread.h>

#define INITIAL_SLEEP_SEC   5
#define NORMAL_SLEEP_SEC    1
#define LAST_SLEEP_SEC      10
#define BUF_SIZE            200
#define NORMAL_REQUEST_SIZE 64
#define MAX_SIZE_IP_HEADER  60
#define NR_MAX_POLL 8192
#define MAX_ITEMS (2 * 1000 * 1000)
// BASEPRD must >= 999
#define BASEPRD 9000

struct record {
	char name[32];
	int fd;
	int seq;
	int latency;
	int epfd;
	//int wating;
	long last_send;
	long lost_start;
	int lost;
	int id;

	//struct sockaddr_in addr_con;
	struct sockaddr_storage addr_con;
    uint8_t snd_buf[BUF_SIZE];
};
struct record *iplist[MAX_ITEMS];


struct per_thread_args {
	int tid;
	struct record **rcd_start;
	int n_elems;
};

int nLine = 0;
int mystartts = 0;

char *g_iface = NULL, *g_host = NULL, *g_local = NULL;
int16_t g_family = AF_INET;
int32_t g_protocol = IPPROTO_ICMP;
uint16_t g_bufsize = BUF_SIZE, g_normal_bufsize = NORMAL_REQUEST_SIZE;
int n_workers = 1;
int n_worker_started = 0;


static int32_t create_bpf_filter6(int32_t bpf_fd,
                                  struct sockaddr_in6 *aRemoteAddr,
                                  uint16_t icmp_id)
{
    struct sock_filter icmp6_filter[] = {
        //Compare type
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ, 0x81, 1, 0),
        //Compare ID
        //BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
        //BPF_JUMP(BPF_JMP | BPF_JEQ, icmp_id, 1, 0),
        //Return values
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, 0xffff),
    };

     struct sock_fprog bpf_code = {
        .len = sizeof(icmp6_filter) / sizeof(icmp6_filter[0]),
        .filter = icmp6_filter,
    };

    if (setsockopt(bpf_fd, SOL_SOCKET, SO_ATTACH_FILTER, &bpf_code, sizeof(bpf_code)) < 0) {
        perror("setsockopt (SO_ATTACH_FILTER)");
        return -1;
    }

    return 0;

}

static int create_bpf_filter(int32_t bpf_fd, struct sockaddr_in *remote,
        uint16_t icmp_id)
{
    struct sock_fprog bpf_code;
    struct sock_filter icmp_filter[] = {
        //Load icmp header offset into index register. Note that this is an
        //inconsistency between v4 and v6
        BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 0),
        //IND is X + K (see /net/core/filter.c), so relative.
        //Store code + type in accumulator
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 0),
        //Check that this is Echo reply (both code and type is 0)
        BPF_JUMP(BPF_JMP | BPF_JEQ, 0x0000, 0, 2),
        //Load ID into accumulator and compare
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ, icmp_id, 1, 0),
        //Return the value stored in K. It is the number of bytes that will be
        //passed on (0 indicates that packet should be ignored).
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, 0xffff),
    };

    memset(&bpf_code, 0, sizeof(bpf_code));
    bpf_code.len = sizeof(icmp_filter) / sizeof(icmp_filter[0]);
    bpf_code.filter = icmp_filter;

    if (setsockopt(bpf_fd, SOL_SOCKET, SO_ATTACH_FILTER, &bpf_code, sizeof(bpf_code)) < 0) {
        perror("setsockopt (SO_ATTACH_FILTER)");
        return -1;
    }

    return 0;
}

static int resolve_addr(char *remote, struct sockaddr_storage *remote_addr, uint8_t family)
{
    struct addrinfo hints, *res = NULL;
    int retval = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;

    retval = getaddrinfo(remote, NULL, &hints, &res);
    if (retval) {
        fprintf(stderr, "getaddrinfo for %s: %s\n", remote, gai_strerror(retval));
        return -1;
    }

    //I only care about the first IP address
    if (res == NULL) {
        fprintf(stderr, "Could not resolve remote\n");
        return -1;
    }

    memcpy(remote_addr, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return 0;
}

static uint16_t calc_csum(const uint8_t *buf, const uint16_t buflen)
{
    int32_t csum = 0;
    uint16_t i = 0;

    //Assumes that buflen is divisible by two
    for (i=0; i<buflen; i+= 2) {
        csum += *((uint16_t*) (buf + i));

        //Add the carry-on back at the right-most bit
        if (csum & 0x10000)
            csum = (csum & 0xFFFF) + (csum >> 16);
    }

    //Add last carry ons
    while (csum >> 16)
        csum = (csum & 0xFFFF) + (csum >> 16);

    //ones complement of the ones complement sum, reason for invert
    csum = ~csum;

    return csum;
}

static ssize_t send_ping(int32_t icmp_fd, uint8_t *buf, uint16_t seq,
        uint16_t sndlen, struct record *e)
{
	int n = 0;
    struct icmphdr *icmph = (struct icmphdr*) buf;
    //We do like nomal ping and keep timer in ICMP payload, gettimeofday is OK
    //for now
    struct timeval *t_pkt = (struct timeval*) (icmph + 1);
    int *p_eid = (int *)(t_pkt + 1);

    icmph->type = ICMP_ECHO;
    icmph->code = 0;

    //Convention, use network byte order
    icmph->un.echo.id = htons(getpid() & 0xffff);
    icmph->un.echo.sequence = htons(seq);
    gettimeofday(t_pkt, NULL);
    //We recycle buffer, so remember to reset checksum
    icmph->checksum = 0;
    *p_eid = e->id;


    //We dont need to use htons here. The reason is that the sum will for
    //example be 0x1cf7 on a LE machine and 0xf71c on a BE machine. This will be
    //stored the same way in memory (LE will put f7 first)
    icmph->checksum = calc_csum(buf, sndlen);

    n = sendto(icmp_fd, buf, sndlen, 0, (struct sockaddr*) &(e->addr_con),
    		sizeof(struct sockaddr));
    if(n < 0) {
    	return n;
    }

    e->last_send = t_pkt->tv_sec * 1000 + t_pkt->tv_usec / 1000;
    return n;
}

static ssize_t send_ping6(int32_t icmp_fd, uint8_t *buf, uint16_t seq,
        uint16_t sndlen, struct record *e)
{
	int n = 0;
    struct icmp6hdr *icmp6h = (struct icmp6hdr*) buf;
    //We do like nomal ping and keep timer in ICMP payload, gettimeofday is OK
    //for now
    struct timeval *t_pkt = (struct timeval*) (icmp6h + 1);
    int *p_eid = (int *)(t_pkt + 1);

    icmp6h->icmp6_type = ICMPV6_ECHO_REQUEST;
    icmp6h->icmp6_code = 0;

    //Convention, use network byte order
    icmp6h->icmp6_dataun.u_echo.identifier = htons(getpid() & 0xffff);
    icmp6h->icmp6_dataun.u_echo.sequence = htons(seq);
    gettimeofday(t_pkt, NULL);

    //We recycle buffer, so remember to reset checksum
    icmp6h->icmp6_cksum = 0;
    *p_eid = e->id;

    e->last_send = t_pkt->tv_sec * 1000 + t_pkt->tv_usec / 1000;

    //We dont need to use htons here. The reason is that the sum will for
    //example be 0x1cf7 on a LE machine and 0xf71c on a BE machine. This will be
    //stored the same way in memory (LE will put f7 first)
    //icmp6h->icmp6_cksum = calc_csum(buf, sndlen);

    n = sendto(icmp_fd, buf, sndlen, 0, (struct sockaddr*) &(e->addr_con),
    		sizeof(struct sockaddr));
    if(n < 0) {
    	return n;
    }

    e->last_send = t_pkt->tv_sec * 1000 + t_pkt->tv_usec / 1000;
    return n;
}

//Will return > 0 on success, <= 0 on failure (recvmsg failed, incorrect
//checksum)
static ssize_t handle_ping_reply(int32_t icmp_fd, uint16_t bufsize,
        int16_t family)
{
    ssize_t numbytes = -1;
    //rcv_buf will contain IP header + ICMP message, so I need to add the
    //maximum size ip header to be sure that request will fit
    uint8_t rcv_buf[bufsize + MAX_SIZE_IP_HEADER];
    uint8_t cmsg_buf[sizeof(struct cmsghdr) + sizeof(struct timeval) + sizeof(int)];

    struct iovec iov;
    struct msghdr msgh;
    struct cmsghdr *cmsg = (struct cmsghdr*) cmsg_buf;

    struct iphdr *iph;
    struct icmphdr *icmph;
    struct icmp6hdr *icmph6;
    uint16_t rcvd_csum = 0;

    double rtt = 0.0;
    struct timeval t_now;
    //We might nead to read timeval from packet
    struct timeval *t_rcv_pkt, *t_snd_pkt;

    memset(rcv_buf, 0, sizeof(rcv_buf));
    iph = (struct iphdr*) rcv_buf;
    //Inconsistency between ICMP and ICMPv6 in kernel, v6 removes IP header
    icmph6 = (struct icmp6hdr*) rcv_buf;

    memset(&iov, 0, sizeof(iov));
    memset(&msgh, 0, sizeof(msgh));

    iov.iov_base = rcv_buf;
    iov.iov_len = sizeof(rcv_buf);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = cmsg;
    msgh.msg_controllen = sizeof(cmsg_buf);

    numbytes = recvmsg(icmp_fd, &msgh, 0);

    if (numbytes <= 0)
        return numbytes;

    if (family == AF_INET) {
        icmph = (struct icmphdr*) (rcv_buf + (iph->ihl*4));
        rcvd_csum = calc_csum((uint8_t*) icmph,
                ntohs(iph->tot_len) - (iph->ihl * 4));
    }

    //For now, just return -1 when checksum is failed
    if (rcvd_csum) {
        fprintf(stderr, "Got ICMP reply with corrupt checksum\n");
        return -1;
    }

    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMP) {
        //Use cmesg data for timestamp if available
        t_rcv_pkt = (struct timeval*) CMSG_DATA(cmsg);
        t_now.tv_sec = t_rcv_pkt->tv_sec;
        t_now.tv_usec = t_rcv_pkt->tv_usec;
    } else {
        gettimeofday(&t_now, NULL);
    }

    //When packet was sent is stored in packet on send
    if (family == AF_INET) {
        t_snd_pkt = (struct timeval*) (icmph + 1);
    }
    else {
        t_snd_pkt = (struct timeval*) (icmph6 + 1);
    }

    // move the addr to the end
    int *ptr_idx = (int *)(t_snd_pkt + 1);
    if(*ptr_idx < 0 || *ptr_idx > nLine) {
    	// WARN
    	fprintf(stderr, "Wrong idx %d in reply\n", *ptr_idx);
    	return -1;
    }
    struct record *e = iplist[*ptr_idx];
    //(t_snd_pkt->tv_sec) * 1000 + t_snd_pkt->tv_usec / 1000;
    if(e->id != *ptr_idx) {
    	// WARN
    	fprintf(stderr, "Wrong idx %d in reply to match %d\n", *ptr_idx, e->id);
    	return -1;
    }

    rtt = ((t_now.tv_sec - t_snd_pkt->tv_sec) * 1000000.0) +
        ((t_now.tv_usec - t_snd_pkt->tv_usec));
	e->latency = rtt;
	//e->last_send = (t_now.tv_sec) * 1000 + t_now.tv_usec / 1000; // fake
	e->lost = 0;
	e->lost_start = -1;

    if (family == AF_INET) {
        //printf("Received seq %u Rtt %.2fms\n", ntohs(icmph->un.echo.sequence),
        //        rtt / 1000.0);
    }
    else {
        //printf("Received seq %u Rtt %.2fms\n",
        //        ntohs(icmph6->icmp6_dataun.u_echo.sequence), rtt / 1000.0);
    }

    return numbytes;
}


static int32_t create_icmp_socket(char *remote, char *local, int16_t family,
        int32_t protocol, const char *iface)
{
	// remote_addr acutally not used in bpf filter
    struct sockaddr_storage remote_addr, local_addr;
    int32_t raw_fd = -1, yes = 1;


    /*
     * move to struct record
    //Resolve the host
    memset(&remote_addr, 0, sizeof(remote_addr));
    if (resolve_addr(remote, &remote_addr, family)) {
        fprintf(stderr, "Could not resolve remote addr\n");
        exit(EXIT_FAILURE);
    }
    */

    if (local) {
        memset(&local_addr, 0, sizeof(local_addr));
        if (resolve_addr(local, &local_addr, family)) {
            fprintf(stderr, "Could not resolve local addr\n");
            exit(EXIT_FAILURE);
        }
    }

    if ((raw_fd = socket(family, SOCK_RAW, protocol)) == -1) {
        perror("socket");
        return -1;
    }

    //argv contains null-terminated strings, so it is safe to pass them to
    //setsockopt
    if (setsockopt(raw_fd, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface)) < 0) {
        perror("setsockopt (SO_BINDTODEVICE)");
        close(raw_fd);
        return -1;
    }

    if (setsockopt(raw_fd, SOL_SOCKET, SO_TIMESTAMP, &yes, sizeof(yes)) < 0) {
        perror("setsockopt (SO_TIMESTAMP)");
        close(raw_fd);
        return -1;
    }

	int rcvbufsize, sndbufsize;
	int sizen = sizeof(int);
	rcvbufsize = 1024 * 1024;
	sndbufsize = 1024 * 1024;
	setsockopt(raw_fd, SOL_SOCKET, SO_RCVBUF, &rcvbufsize, sizen);
	setsockopt(raw_fd, SOL_SOCKET, SO_SNDBUF, &sndbufsize, sizen);
	//getsockopt(raw_fd, SOL_SOCKET, SO_RCVBUF, &rcvbufsize, &sizen);
	//getsockopt(raw_fd, SOL_SOCKET, SO_SNDBUF, &sndbufsize, &sizen);
	//printf("Socket receive buf size = %d, send buf size = %d\n", rcvbufsize, sndbufsize);


    if (local && bind(raw_fd, (const struct sockaddr*) &local_addr, sizeof(local_addr))) {
        perror("bind");
        close(raw_fd);
        return -1;
    }

    /* do not use connect
    if (connect(raw_fd, (const struct sockaddr *) &remote_addr, sizeof(remote_addr))) {
        perror("socket");
        close(raw_fd);
        return -1;
    }
    */

    //Use lowest two bytes of getpid as ID
    //The reason we dont have a htons/ntohs here, is that we simply provide the
    //value we want to match. That is the same, irrespective of byte order. Note
    //that a htons is still required when inserting the pid into the packet.
    //Othwerwise, a LE machine will store the value inverse of what we expect

    if (family == AF_INET) {
        if (create_bpf_filter(raw_fd, (struct sockaddr_in*) &remote_addr,
                    getpid() & 0xFFFF) < 0)
            exit(EXIT_FAILURE);
    } else {
         if (create_bpf_filter6(raw_fd, (struct sockaddr_in6*) &remote_addr,
                    getpid() & 0xFFFF) < 0)
            exit(EXIT_FAILURE);
    }

    return raw_fd;
}


// ~/Documents/bugIP.txt
void genList(char *fileName) {
	FILE *fp = fopen(fileName, "r");
	char buf[32];
	while(fgets(buf, 32, fp) != NULL) {
		buf[31] = '\0';
		int len = strlen(buf);
		buf[len - 1] = '\0';
		struct record *e = malloc(sizeof(struct record));
		// assert
		strncpy(e->name, buf, len);
		e->latency = -1;
		e->fd = -1;
		e->seq = -1;
		e->epfd = -1;
		e->last_send = -1;
		e->lost_start = -1;
		e->lost = 0;
		e->id = nLine;
		iplist[nLine] = e;
		nLine++;
	}
	fclose(fp);
	fprintf(stdout, " *** Got %d records\n", nLine);
}

int send_ping_wrapper(struct record *e, int family, int bufsize) {
	uint8_t *snd_buf = e->snd_buf;
	int raw_fd = e->fd;
	int* pseq = &(e->seq);
	int numbytes = -1;

	//Set IP header to point correctly
	memset(snd_buf, 0, BUF_SIZE);
	*pseq = 0;
	if (family == AF_INET) {
		numbytes = send_ping(raw_fd, snd_buf, *pseq, bufsize, e);
	}
	else {
		numbytes = send_ping6(raw_fd, snd_buf, *pseq, bufsize, e);
	}

	//Send first packet and start timer
	if (numbytes < 0) {
		perror("sendto");
		return -1;
	} else {
		//fprintf(stdout, "Sent pkt with seq %u\n", seq);
		e->seq ++;
		e->latency = -1;
	}

	return numbytes;
}

void * do_work(void *data) {

	struct per_thread_args *targ = (struct per_thread_args *)data;
	int i = 0, raw_fd = -1;

	raw_fd = create_icmp_socket(NULL, g_local, g_family, g_protocol, g_iface);
	if (raw_fd == -1) {
		exit(EXIT_FAILURE);
	}

	struct timeval tv;
    gettimeofday(&tv, NULL);
    long ts1 = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    long ts2 = 0;
    for(i = 0; i < targ->n_elems; i++) {
		struct record *e = (targ->rcd_start)[i];
		uint16_t seq = (uint16_t) e->seq;
	    ssize_t numbytes = 0;

		e->fd = raw_fd;

	    //Resolve the host, sa_family_t
	    memset(&(e->addr_con), 0, sizeof(struct sockaddr_storage));
	    if (resolve_addr(e->name, &(e->addr_con), g_family)) {
	        fprintf(stderr, "Could not resolve remote addr\n");
	        exit(EXIT_FAILURE);
	    }

		e->latency = 1; // fake
		e->last_send = ts1 - (i % (BASEPRD - 999)); // fake
		e->lost = 0;
    }

    gettimeofday(&tv, NULL);
    ts2 = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	__atomic_fetch_add(&(n_worker_started), 1, __ATOMIC_SEQ_CST);
	fprintf(stdout, "Thread %03d finished icmp socket creation in %d ms\n", targ->tid,
			ts2 - ts1);

	while (1) {
		handle_ping_reply(raw_fd, g_bufsize, g_family);
	}
}


static void usage()
{
    printf("Supported arguments\n");
    printf("\t-6 : IPv6 ping\n");
    printf("\t-i : interface to bind to (required)\n");
    printf("\t-n : number of threads\n");
    printf("\t-l : local IP to use\n");
    printf("\t-c : number of packets to send (default: run indefinitely\n");
    printf("\t-f : the IPList file\n");
    printf("\t-t : how long to wait between packets (default: 1 sec)\n");
    printf("\t-s : inital packet size (default: 200 byte)\n");
    printf("\t-p : normal packet size (default: 64 byte)\n");
    printf("\t-h : this menu\n");
}

int main(int argc, char *argv[])
{
    int32_t raw_fd = 0, opt = 0, count = 0, tmp_val = 0;
    uint8_t first_sleep = INITIAL_SLEEP_SEC, normal_sleep = NORMAL_SLEEP_SEC;
    int i = 0;

    while ((opt = getopt(argc, argv, "i:c:n:f:t:s:p:l:h6")) != -1) {
        switch (opt) {
        case '6':
            g_family = AF_INET6;
            g_protocol = 0x3A;
            break;
        case 'i':
        	g_iface = strdup(optarg);
            break;
        case 'n':
        	n_workers = atoi(optarg);
            if (n_workers < 1 || n_workers > 512) {
                fprintf(stderr, "-n is invalid (0 < t <= 512)\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'c':
            count = atoi(optarg);
            break;
        case 'l':
        	g_local = strdup(optarg);
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
            break;
        case 'f':
            genList(optarg);
            if(n_workers > nLine) {
            	n_workers = nLine;
            }
            break;
        case 't':
            tmp_val = atoi(optarg);

            if (tmp_val < 0 || tmp_val > UINT8_MAX) {
                fprintf(stderr, "-t is invalid (0<t<256)\n");
                exit(EXIT_FAILURE);
            }

            normal_sleep = tmp_val;
            break;
        case 's':
            tmp_val = atoi(optarg);

            if (tmp_val < 8 || tmp_val > BUF_SIZE) {
                fprintf(stderr, "-s is invalid (8<s<200\n");
                exit(EXIT_FAILURE);
            }

            g_bufsize = tmp_val;
            break;
        case 'p':
            tmp_val = atoi(optarg);

            if (tmp_val < 8 || tmp_val > NORMAL_REQUEST_SIZE) {
                fprintf(stderr, "-p is invalid (8<p<64)\n");
                exit(EXIT_FAILURE);
            }

            g_normal_bufsize = tmp_val;
            break;
        case '?':
        default:
            fprintf(stderr, "Unknown argument\n");
            usage();
            exit(EXIT_FAILURE);
        }
    }

    if (g_iface == NULL || nLine < 1) {
        fprintf(stderr, "Missing argument\n");
        usage();
        exit(EXIT_FAILURE);
    }

    //normal bufsize must always be smaller than bufsize
    if ((g_bufsize != BUF_SIZE || g_normal_bufsize != NORMAL_REQUEST_SIZE) &&
            g_normal_bufsize > g_bufsize) {
        fprintf(stderr, "Normal bufsize is larger than maximum bufsize\n");
        exit(EXIT_FAILURE);
    }

    //strlen excludes 0 byte, so strlen(iface) has to be less than IFNAMSIZ
    //Kernel checks length > IFNAMSIZ - 1
    if (strlen(g_iface) >= IFNAMSIZ) {
        fprintf(stderr, "Interface name is longer than limit\n");
        exit(EXIT_FAILURE);
    }



    fprintf(stdout, "Interface: %s Threads: %d Hosts: %d\n", g_iface, n_workers, nLine);


    int nelems = (nLine + n_workers - 1) / n_workers;
    for(i = 0; i < n_workers; i++) {

    	struct per_thread_args *targ = malloc(sizeof(struct per_thread_args));
    	if(targ == NULL) {
            perror("no mem");
            return -1;
    	}

        targ->tid = i;
		targ->rcd_start = &(iplist[i * nelems]);
        targ->n_elems = nelems;
        if(i * nelems + nelems > nLine) {
        	targ->n_elems = nLine - i * nelems;
        }

        pthread_t tid;
    	if (0 > pthread_create(&tid, NULL, do_work, (void *)targ)) {
    		perror("could not create IO thread");
    		return -1;
    	}
    	char pname[16];
    	sprintf(pname, "pworker-%03d", i);
    	pthread_setname_np(tid, pname);
    }


    // wait for all thread initialized
    while(n_worker_started < n_workers) {
    	usleep(20000);
    }
    fprintf(stdout, " *** Go !\n");



    int nloop = 0;
    long n_ping_send = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long last_report_ts = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    mystartts = tv.tv_sec;
	char tmpbuf[512];
	FILE *fp = NULL;

    while(1) {
    	usleep(20000);
    	nloop++;
        int dump = 0;
    	int nlost1 = 0;
    	int nlost2 = 0;
    	int nlost3 = 0;
    	int nlost4 = 0;
        gettimeofday(&tv, NULL);
        long ts = tv.tv_sec * 1000 + tv.tv_usec / 1000;

        // do dump every 10s
		if(ts > last_report_ts + 10000) {
			// do dump
			dump = 1;
			fp = fopen("/dev/shm/ping_lostconn_5min.txt", "w");
			int len = snprintf(tmpbuf, sizeof(tmpbuf), "### kit up %d since %d\n",
					tv.tv_sec - mystartts, mystartts);
			fwrite(tmpbuf, len, 1, fp);
		}

    	for(i = 0; i < nLine; i++) {
    		struct record *e = iplist[i];
    		if(e->last_send + BASEPRD < ts) {
    			if(e->latency == -1) {
    				// first time lost
        			if(e->lost == 0) {
        				e->lost_start = ts;
        			}
    				e->lost++;
    			}
    			send_ping_wrapper(e, g_family, g_bufsize);
    			n_ping_send++;
    		} else {
    			// not expired
    		}
			if(e->lost > 0) {
    			nlost1++;
			}
			if(e->lost > 5) {
    			nlost2++;
			}
			if(e->lost > 29) {
    			nlost3++;
    			if(dump && fp) {
					int len = snprintf(tmpbuf, sizeof(tmpbuf), "%s %ld %ld\n", e->name,
							(ts - e->lost_start) / 1000, e->lost_start / 1000);
					fwrite(tmpbuf, len, 1, fp);
    			}
			}
			if(e->lost > 89) {
    			nlost4++;
			}
    	}

    	if(dump) {
    		fprintf(stdout, " *** %d.%03d: %d/%d/%d/%d out of %d not reply, total %ld send, loop %d\n",
    				tv.tv_sec, tv.tv_usec / 1000, nlost1, nlost2, nlost3, nlost4,
					nLine, n_ping_send, nloop);
    		nloop = 0;
    		n_ping_send = 0;
    		// do not use ts to update last_report_ts;
    		last_report_ts += 10000;
    		if(fp) {
    			int len = snprintf(tmpbuf, sizeof(tmpbuf), "### dump end\n");
    			fwrite(tmpbuf, len, 1, fp);
    			fclose(fp);
    			fp = NULL;
    		}
    	}
    }


	exit(0);
    //exit(EXIT_FAILURE);
}

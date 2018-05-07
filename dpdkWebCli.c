
/* vim: set ts=4: */
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <time.h>

#include "ff_config.h"
#include "ff_api.h"
#include "ff_epoll.h"


#define MAX_EVENTS 512
#define PATH_MAX 1024
#define APR_SIZE_T_FMT "lu"

struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];

int epfd;
int sockfd;
int proc_id = 0;
int init_done = 0;

uint64_t n_con = 0;
uint64_t n_req = 0;
uint64_t n_rcv = 0;
uint64_t n_cls = 0;

struct data {
    int64_t starttime;/* start time of connection */
    int64_t waittime; /* between request and reading response */
    int64_t ctime;    /* time to connect */
    int64_t time;     /* time for connection */
};


int verbosity = 0;      /* no verbosity by default */
int recverrok = 0;      /* ok to proceed after socket receive errors */
enum {NO_METH = 0, GET, HEAD, PUT, POST, CUSTOM_METHOD} method = NO_METH;
const char *method_str[] = {"bug", "GET", "HEAD", "PUT", "POST", ""};
int send_body = 0;      /* non-zero if sending body with request */
int requests = 1;       /* Number of requests to make */
int heartbeatres = 100; /* How often do we say we're alive */
int concurrency = 1;    /* Number of multiple requests to make */
int percentile = 1;     /* Show percentile served */
int nolength = 0;       /* Accept variable document length */
int confidence = 1;     /* Show confidence estimator and warnings */
int tlimit = 0;         /* time limit in secs */
int keepalive = 0;      /* try and do keepalive connections */
int windowsize = 0;     /* we use the OS default window size */
char servername[1024];  /* name that server reports */
char *hostname;         /* host name from URL */
const char *host_field;       /* value of "Host:" header field */
const char *path;             /* path name */
char *postdata;         /* *buffer containing data from postfile */
size_t postlen = 0; /* length of data to be POSTed */
char *content_type = NULL;     /* content type to put in POST header */
const char *cookie,           /* optional cookie line */
           *auth,             /* optional (basic/uuencoded) auhentication */
           *hdrs;             /* optional arbitrary headers */
uint16_t port;        /* port number */
char *proxyhost = NULL; /* proxy host name */
int proxyport = 0;      /* proxy port */
const char *connecthost;
const char *myhost;
uint16_t connectport;
const char *gnuplot;          /* GNUplot file */
const char *csvperc;          /* CSV Percentile file */
const char *fullurl;
const char *colonhost;
int isproxy = 0;
//apr_interval_time_t aprtimeout = apr_time_from_sec(30); /* timeout value */

/* overrides for ab-generated common headers */
const char *opt_host;   /* which optional "Host:" header specified, if any */
int opt_useragent = 0;  /* was an optional "User-Agent:" header specified? */
int opt_accept = 0;     /* was an optional "Accept:" header specified? */
 /*
  * XXX - this is now a per read/write transact type of value
  */

int use_html = 0;       /* use html in the report */
const char *tablestring;
const char *trstring;
const char *tdstring;

static size_t doclen = 0;     /* the length the document should be */
static int64_t totalread = 0;    /* total number of bytes read */
static int64_t totalbread = 0;   /* totoal amount of entity body read */
static int64_t totalposted = 0;  /* total number of bytes posted, inc. headers */
static int started = 0;           /* number of requests started, so no excess */
static int done = 0;              /* number of requests we have done */
static int doneka = 0;            /* number of keep alive connections done */
static int good = 0, bad = 0;     /* number of good and bad requests */
static int epipe = 0;             /* number of broken pipe writes */
static int err_length = 0;        /* requests failed due to response length */
static int err_conn = 0;          /* requests failed due to connection drop */
static int err_recv = 0;          /* requests failed due to broken read */
static int err_except = 0;        /* requests failed due to exception */
static int err_response = 0;      /* requests with invalid or non-200 response */

/* global request (and its length) */
static char _request[8192];
static char *request = _request;
static size_t reqlen;

/* one global throw-away buffer to read stuff into */
char buffer[8192];

/* interesting percentiles */
int percs[] = {50, 66, 75, 80, 90, 95, 98, 99, 100};

//struct connection *con;     /* connection array */
struct data *stats;         /* data for each request */

/* simple little function to write an error string and exit */
static void err(const char *s)
{
    fprintf(stderr, "%s failed %d: %s\n", s, errno, strerror(errno));
    if (done)
        printf("Total of %d requests completed\n" , done);
    exit(1);
}

static void *xmalloc(size_t size)
{
    void *ret = malloc(size);
    if (ret == NULL) {
        fprintf(stderr, "Could not allocate memory (%"
                APR_SIZE_T_FMT" bytes)\n", size);
        exit(1);
    }
    return ret;
}

static void *xcalloc(size_t num, size_t size)
{
    void *ret = calloc(num, size);
    if (ret == NULL) {
        fprintf(stderr, "Could not allocate memory (%"
                APR_SIZE_T_FMT" bytes)\n", size*num);
        exit(1);
    }
    return ret;
}

static inline unsigned long long time_ns(struct timespec* const ts) {
        if (clock_gettime(CLOCK_REALTIME, ts)) {
                exit(1);
        }
        return ((unsigned long long) ts->tv_sec) * 1000000000LLU
                        + (unsigned long long) ts->tv_nsec;
}


        unsigned long long start_ns = 0;
        struct timespec ts;
		unsigned long long delta = 0;


static struct sockaddr_in svr_addr;
//static const char *r = "GET /90.html HTTP/1.0\nConnection: Keep-Alive\nHost: 10.10.23.58\nUser-Agent: AB\nAccept: */*\n\n";
static const char *r = NULL;
static const char *r1 = "GET /2.html HTTP/1.1\nConnection: Close\nHost: 192.168.2.3\n\n";
static const char *r2 = "GET /2.html HTTP/1.1\nConnection: Close\nHost: 192.168.3.3\n\n";
static size_t len_r;
static int nclose, nopen, nconn, nsend, nrecv;
static const int conn_max = 50;
static const int nsend_max = 1000000;
int cnt = 0;
int idleloop = 0;


#define LINUX_SO_LINGER       13


int reconn() {

	int cnt1= 0;
    int i, len, rc, on = 1, off = 0;

    //while (nconn < conn_max) {
        sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            err("ff_socket()\n");
        }

        if (ff_ioctl(sockfd, FIONBIO, &on) == -1)
            err("ff_ioctl()");
/*
        if (ff_setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1)
            err("ff_setsockopt()");
        if (ff_setsockopt(sockfd, SOL_SOCKET, LINUX_SO_LINGER, // struct linger) == -1)
            err("ff_setsockopt()");
*/

again:
        if (ff_connect(sockfd, (const struct linux_sockaddr*)&svr_addr, sizeof(svr_addr)) == -1)
        {
            if (errno != EINPROGRESS) {
                err("connect()");
                goto again;
            }
        }
        nconn++;

//        if (++nopen % 1000000 == 0)
//            fprintf(stderr, "open %d times\n", nopen);
    
        ev.data.fd = sockfd;
        ev.events = EPOLLOUT;
        if(ff_epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
			fprintf(stderr ,"CNT %d sockfd %d ff_epoll_ctl failed\n", cnt++, cnt1++, sockfd);
		}

		n_con++;
	
		//if((n_rcv > 0) && ((n_rcv % 1000000) == 0)) {
		if((n_con % 1000000) == 0) {
			delta = (time_ns(&ts) - start_ns) / 1000;
			printf(" * Workload %02d: %ld %ld %ld %ld %.03f in %.02f seconds\n", proc_id, n_con, n_req, 
				n_rcv, n_cls, (double)n_rcv / (double)delta * 1000000.0f, (double)delta / 1000000.0f);
			n_con = 0; n_req = 0; n_rcv = 0; n_cls = 0;
			start_ns = time_ns(&ts);
		}
		//fprintf(stderr ,"CNT %d %d sockfd %d connected\n", cnt++, cnt1++, sockfd);

		/*
		if(cnt > 100) {
			exit(0);
		}
		*/

    //}
}



static int loop(void *arg)
{
    /* Wait for events to happen */
    int i, len, rc, on = 1, off = 0;
    int nevents;

	if(proc_id == 0) {
		//return;
	}

my_entry:

	if(!init_done) {
		while (conn_max > nconn) {
			reconn();
		}
		init_done = 1;
	}


    nevents = ff_epoll_wait(epfd, events, MAX_EVENTS, -1);
	if(nevents > 0) {
 		//fprintf(stderr, " * Get %d events, idleloop = %d\n", nevents, idleloop);
		;
	} else {
		idleloop++;
		//goto my_entry;
	}

	size_t readlen = 0 ;
    for (i = 0; i < nevents; ++i) {

		//fprintf(stderr, " * Evt %d with flag %X\n", i, events[i].events);
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {

			if(events[i].events & EPOLLHUP) {
				readlen = ff_read(events[i].data.fd, buffer, sizeof(buffer));
				//fprintf(stderr, "HUP recv %d bytes\n", readlen);
				
				if(readlen > 0) {
					ev.data.fd = events[i].data.fd;
					ev.events = EPOLLIN | EPOLLET;
					ff_epoll_ctl(epfd, EPOLL_CTL_MOD, events[i].data.fd, &ev);
					n_rcv++;
					continue;
				}

				// close on 0 or -1

			}

            ff_epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
            ff_close(events[i].data.fd);
			n_cls++;
            --nconn;
			//reconn();
			continue;
            //fprintf(stderr, "    close fd %d in %dth event, remaining conn: %d\n", events[i].data.fd, i, nconn);
//            if (++nclose % 1000000 == 0)
//                fprintf(stderr, "EPOLLERR close %d times\n", nclose);
        } if (events[i].events & EPOLLOUT) {
            len = ff_write(events[i].data.fd, r, len_r);
			//fprintf(stderr, "OUT write %d bytes\n", len);
//            if (++nsend % 1000000 == 0)    
//                fprintf(stderr, "send() %d times\n", nsend);

            ev.data.fd = events[i].data.fd;
            //ev.events &= ~EPOLLOUT;
            ev.events = EPOLLIN | EPOLLET;
            ff_epoll_ctl(epfd, EPOLL_CTL_MOD, events[i].data.fd, &ev);
			n_req++;

        } if (events[i].events & EPOLLIN) {
            size_t readlen = ff_read(events[i].data.fd, buffer, sizeof(buffer));
			//fprintf(stderr, "IN recv %d bytes\n", readlen);
            if (readlen <= 0) {
                ff_epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                ff_close(events[i].data.fd);
                --nconn;
				reconn();
				fprintf(stderr, "Close for read %d bytes \n", readlen);
                //fprintf(stderr, "    close fd %d in %dth event, remaining conn: %d\n", events[i].data.fd, i, nconn);
//                if (++nclose % 1000000 == 0)
//                    fprintf(stderr, "EPOLLIN: close %d times\n", nclose);
            } else {
				// wait for srv to close ? recv FIN?
                ev.data.fd = events[i].data.fd;
                ev.events = EPOLLIN | EPOLLET;
                ff_epoll_ctl(epfd, EPOLL_CTL_MOD, events[i].data.fd, &ev);
				n_rcv++;
            }

//            if (++nrecv % 1000000 == 0)    
//                fprintf(stderr, "recv() %d times\n", nrecv);
        }
    }

	if((conn_max - nconn) < 10) {
		//fprintf(stderr, "Unlikely: %d/%d %d\n", nconn, conn_max, nevents);
		return;
	}

    while (conn_max > nconn) {
		//fprintf(stderr, "Unlikely: %d/%d %d\n", nconn, conn_max, nevents);
		reconn();
	}


}

static void ab_init(int argc, char *argv[])
{  
    int ff_argc = 4;
    int i, rc;

    char **ff_argv = malloc(sizeof(char *)*ff_argc);
    for (i = 0; i < ff_argc; i++) {
        ff_argv[i] = malloc(sizeof(char)*PATH_MAX);
    }

    sprintf(ff_argv[0], "apr");
    sprintf(ff_argv[1], "--conf=%s", argv[1]);
    sprintf(ff_argv[3], "--proc-type=%s", argv[2]);
    sprintf(ff_argv[2], "--proc-id=%d", atoi(argv[3]));

	proc_id = atoi(argv[3]);

    rc = ff_init(ff_argc, ff_argv);
    assert(0 == rc);

    for (i = 0; i < ff_argc; i++) {
        free(ff_argv[i]);
    }

    free(ff_argv);
}

int main(int argc, char *argv[])
{
    myhost = NULL; /* 0.0.0.0 or :: */
    ab_init(argc, argv);

    bzero(&svr_addr, sizeof(svr_addr));
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_port = htons(80);

	char IPs[16];
	char URLs[128];
	sprintf(IPs, "10.10.23.58");
	sprintf(URLs, "GET /2.html HTTP/1.1\nConnection: Close\nHost: 10.10.23.58\n\n");
	//sprintf(IPs, "192.168.%d.3", proc_id + 2);
	//sprintf(URLs, "GET /2.html HTTP/1.1\nConnection: Close\nHost: 192.168.%d.3\n\n", proc_id + 2);
	//sprintf(IPs, "192.168.%d.3", 2);
	//sprintf(URLs, "GET /2.html HTTP/1.1\nConnection: Close\nHost: 192.168.%d.3\n\n", 2);
	r = URLs;
    svr_addr.sin_addr.s_addr = inet_addr(IPs);
    len_r = strlen(r);


    assert((epfd = ff_epoll_create(1024)) > 0);
	fprintf(stderr, "\n\n %s: Lanch nconn %d, proc_id is %d\nWith Remote IP %s\nWith HTTP Request %s",
		 argv[0], nconn, proc_id, IPs, URLs);

	start_ns = time_ns(&ts);
    ff_run(loop, (void *)(unsigned long) proc_id);

    return 0;
}


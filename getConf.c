root@n3-021-218:~# cat confUpdateClient.c
/*
 * tcpclient.c - A simple TCP client
 * usage: tcpclient <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>

#define BUFSIZE 1024
#define PING_INTERVAL 10
//////

#define HOST_MAX (5)
#define FAIL_MUTE_COUNT (5)

char *uplink_list[] = {
                "10.10.10.10",
                "10.188.188.188"
                // domain name maybe in future
};

int cur_uplink_idx = 0;
int retry_counter = 0;
int retry_delay = 10;


// only IPv4 support for the code
static int try_connect(const char *text_ip, short srv_port, uint32_t local_ip, short local_port, int *p_sock_desc) {

        int one = 1;

        struct sockaddr_in serv_addr;
        struct sockaddr_in cli_addr;

        struct timeval tv;
        // every 5 min
        tv.tv_sec = PING_INTERVAL;
        tv.tv_usec = 0;

        if(*p_sock_desc >= 0) {
                close(*p_sock_desc);
        }

        if((*p_sock_desc = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            fprintf(stderr, "Failed creating socket\n");
            return -20;
        }

        setsockopt(*p_sock_desc, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(*p_sock_desc, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof (int));
        setsockopt(*p_sock_desc, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof (int));
        setsockopt(*p_sock_desc, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);


        bzero((char *) &serv_addr, sizeof (serv_addr));
        bzero((char *) &cli_addr, sizeof (cli_addr));

        serv_addr.sin_family = AF_INET;
        cli_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = inet_addr(text_ip);
        cli_addr.sin_addr.s_addr = local_ip;
        serv_addr.sin_port = htons(srv_port);
        cli_addr.sin_port = htons(local_port);

        if(bind(*p_sock_desc, (struct sockaddr*) &cli_addr, sizeof(cli_addr)) == -1) {
                fprintf(stderr, "Failed to bind local ip and port, use abitary port\n");
                /**
                close(*p_sock_desc);
                *p_sock_desc = -1;
                return -30;
                */
        }

        if (connect(*p_sock_desc, (struct sockaddr *) &serv_addr, sizeof (serv_addr)) < 0) {
                fprintf(stderr, "Failed to connect to server\n");
                close(*p_sock_desc);
                *p_sock_desc = -1;
                return -40;
        }

        return 0;
}
///////


int worker_thread() {
    int sockfd = -1, portno = 50413, n = 0, ret = 0;
    struct pollfd pfd;

    char buf[BUFSIZE];

        struct timespec t;
        struct timeval tv;
        cur_uplink_idx = -1;


try_conn:
        cur_uplink_idx = (cur_uplink_idx + 1) % HOST_MAX;
        if(sockfd > 0) {
                close(sockfd);
                sockfd = -1;
        }
    ret = try_connect(uplink_list[cur_uplink_idx], (short)portno, INADDR_ANY, 100, &sockfd);
    if(ret < 0) {
        retry_counter++;
        if(retry_counter == FAIL_MUTE_COUNT) {
                // silent for a while
                retry_counter = 0;
                sleep(retry_delay);
        }
        goto try_conn;
    }
    pfd.fd = sockfd;
    pfd.events = POLLIN;

wait_data:
        //ret = write(sockfd, "HELLO\n", 6);
        //if(ret < 6) {
        //      goto try_conn;
        //}
        // should always block here
        ret = poll(&pfd, 1, 999 * PING_INTERVAL);
        switch (ret) {
                case -1:
                        // Error
                        goto try_conn;
                case 0:
                        // timeout, keep alive, send ping
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        gettimeofday(&tv, NULL);

                        n = snprintf(buf, 128, "TICK %llu %llu %llu %llu %d\n",
                                        (unsigned long long)(tv.tv_sec), (unsigned long long)(tv.tv_usec),
                                        (unsigned long long)t.tv_sec, (unsigned long long)t.tv_nsec, cur_uplink_idx);

                        // !!! assert n > 0
                        ret = write(sockfd, buf, n);
                        if(ret < 1) {
                                goto try_conn;
                        }
                        // connection seems alive, turn to wait data for next PING_INTERVAL
                        break;
                default:
                        ret = read(sockfd, buf, BUFSIZE); // get your data
                    if (ret < 0) {
                        goto try_conn;
                    }
                        // ret == 0 : remote close the conn
                    if(ret == 0) {
                                // let logic set cur_uplink_idx to 0 for retry
                                cur_uplink_idx = -1;
                                goto try_conn;
                    }
                    // rc > 0, discard all the remain data in buf
                    // check if '\n' terminated
                    if(buf[ret] != '\n') {
                        // warning !!!
                    }
                    buf[ret] = '\0';
                    printf("Echo from server: %s", buf);
                    fflush(stdout);
                        break;
        }
        goto wait_data;

    // should not reached
    close(sockfd);
    sockfd = -1;
}

int main(int argc, char **argv) {

    /* check command line arguments */
        worker_thread();

    return 0;
}

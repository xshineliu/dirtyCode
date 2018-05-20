#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <libaio.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NBYTES 4096
#define NBUF   16


int main(int argc, char *argv[])
{
        int fd, rc, j, k, nbytes = NBYTES, maxevents = NBUF;
        char *buf[NBUF], *filename = "/dev/sda";
        struct iocb *iocbray[NBUF], *iocb;
        off_t offset;
        io_context_t ctx = 0;
        struct io_event events[2 * NBUF];
        struct timespec timeout = { 10, 0 };

        /* open or create the file and fill it with a pattern */

        if (argc > 1)
                filename = argv[1];

        printf("opening %s\n", filename);

        /* notice opening with these flags won't hurt a device node! */

        if ((fd = open(filename, O_RDONLY | O_DIRECT,
                       S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)) < 0) {
                printf("couldn't open %s, ABORTING\n", filename);
                exit(-1);
        }

        /* write initial data out, clear buffers, allocate iocb's */

        for (j = 0; j < NBUF; j++) {
                /* no need to zero iocbs; will be done in io_prep_pread */
                iocbray[j] = malloc(sizeof(struct iocb));
                posix_memalign((void **)(buf + j), 4096, nbytes);
        }
        printf("\n");

        /* prepare the context */

        rc = io_setup(maxevents, &ctx);
        printf(" rc from io_setup = %d\n", rc);

        /* (async) read the data from the file */

        printf(" reading initial data from the file:\n");

        for (j = 0; j < NBUF; j++) {
                iocb = iocbray[j];
                offset = j * nbytes;
                io_prep_pread(iocb, fd, (void *)buf[j], nbytes, offset);
                rc = io_submit(ctx, 1, &iocb);
        }

        usleep(1000);

        /* sync up and print out the readin data */
        j = 0;
        while (j < NBUF) {
                rc = io_getevents(ctx, 1, NBUF, events, &timeout);
                if(rc > 0) {
                        j += rc;
                        printf(" rc from io_getevents on the read = %d\n", rc);
                }
        }

        /* clean up */
        rc = io_destroy(ctx);
        close(fd);
        exit(0);
}

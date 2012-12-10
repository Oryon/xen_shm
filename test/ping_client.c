#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../client_lib.h"

#define PACKET_SIZE 10
#define NB  500

int
main(int argc, char *argv[])
{
    int retval;
    int i;
    ssize_t len;
    in_port_t port;
    uint8_t page_count;
    struct in_addr addr;
    xen_shm_pipe_p receive_fd;
    xen_shm_pipe_p send_fd;
    struct timespec in_stamp;
    struct timespec out_stamp;

    uint8_t noise[PACKET_SIZE];

    receive_fd = NULL;
    send_fd = NULL;

    if (argc > 4) {
        printf("Too many arguments\n");
        return -1;
    }

    if (argc < 4) {
        printf("Not enough arguments (addr, port, pages)\n");
        return -1;
    }

    retval = inet_pton(AF_INET, argv[1], &addr);
    if (retval != 1) {
        printf("Bad address\n");
        return -1;
    }

    if (sscanf(argv[2], "%"SCNu16, &port) != 1) {
        printf("Bad port\n");
        return -1;
    }

    if (sscanf(argv[3], "%"SCNu8, &page_count) != 1) {
        printf("Bad page number\n");
        return -1;
    }

    retval = init_pipe(port, &addr, &receive_fd, &send_fd, page_count);
    if (retval != 0) {
        printf("Unable to init pipe\n");
        return -1;
    }

    while(1) {
        clock_gettime(CLOCK_REALTIME, &out_stamp);
        for (i = 0; i < NB; ++i) {
            len = xen_shm_pipe_write_all(send_fd, noise, PACKET_SIZE);
            if (len < 0) {
                printf("Unable to send\n");
                perror("xen_shm_pipe_write_all");
                return -1;
            }
            xen_shm_pipe_flush(send_fd);
            len = xen_shm_pipe_read_all(receive_fd, noise, PACKET_SIZE);
            if (len < 0) {
                printf("Unable to receive\n");
                perror("xen_shm_pipe_read_all");
                return -1;
            }
        }
        clock_gettime(CLOCK_REALTIME, &in_stamp);
        printf("Sent at %ld.%09ld\n", out_stamp.tv_sec, out_stamp.tv_nsec);
        printf("Received at %ld.%09ld\n", in_stamp.tv_sec, in_stamp.tv_nsec);
    }
    return 0;
}

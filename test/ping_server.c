#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../server_lib.h"

#define PACKET_SIZE 10

static void*
ping_server (struct xen_shm_server_data* data)
{
    uint8_t noise[PACKET_SIZE];
    ssize_t len;

    while (!data->stop) {
        len = xen_shm_pipe_read_all(data->receive_fd, &noise, PACKET_SIZE);
        if (len < 0) {
            printf("Unable to receive\n");
            perror("xen_shm_pipe_read_all");
            return NULL;
        }
        len = xen_shm_pipe_write_all(data->send_fd, &noise, PACKET_SIZE);
        if (len < 0) {
            printf("Unable to send\n");
            perror("xen_shm_pipe_write_all");
            return NULL;
        }
        xen_shm_pipe_flush(data->send_fd);
    }
    return NULL;
}



int
main(int argc, char *argv[])
{
    in_port_t port;
    uint8_t page_count;

    if (argc > 3) {
        printf("Too many arguments\n");
        return -1;
    }

    if (argc < 3) {
        printf("Not enough arguments (port, pages)\n");
        return -1;
    }

    if (sscanf(argv[1], "%"SCNu16, &port) != 1) {
        printf("Bad port\n");
        return -1;
    }

    if (sscanf(argv[2], "%"SCNu8, &page_count) != 1) {
        printf("Bad page number\n");
        return -1;
    }

    return run_server(port, page_count, ping_server, NULL);
}


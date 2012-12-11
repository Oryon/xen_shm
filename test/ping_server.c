#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../server_lib.h"


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

    return run_server(port, page_count, xen_shm_handler_ping_server, NULL);
}


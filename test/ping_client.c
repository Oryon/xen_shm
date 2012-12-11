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
#include "../handler_lib.h"

#define PACKET_SIZE 10
#define NB  500

int
main(int argc, char *argv[])
{
    int retval;
    in_port_t port;
    uint8_t page_count;
    struct in_addr addr;
    struct xen_shm_handler_data hdrl_data;
    pthread_t thread_info;


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

    retval = run_client_thread(port, &addr, page_count, xen_shm_handler_ping_client,  &hdrl_data, &thread_info);

    if(retval != 0) {
        return retval;
    }

    pthread_join(thread_info, NULL);

    return 0;
}


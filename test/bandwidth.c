#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "../client_lib.h"
#include "../server_lib.h"

void usage(void);
int server(int argc, char **argv);
int client(int argc, char **argv);

void usage(void) {
    printf("server <port> <pages_count> <buffer_size> \n");
    printf("client <addr> <port> <pages_count> <buffer_size> <client_count> <sec_between_clients_start>\n");
}

int server(int argc, char **argv) {
    in_port_t port;
    uint8_t page_count;
    uint32_t buffer_size;
    struct xen_shm_handler_transfert tr;

    if (argc > 5) {
        printf("Too many arguments\n");
        usage();
        return -1;
    }

    if (argc < 5) {
        printf("Not enough arguments\n");
        usage();
        return -1;
    }

    if (sscanf(argv[2], "%"SCNu16, &port) != 1) {
        printf("Bad port\n");
        usage();
        return -1;
    }

    if (sscanf(argv[3], "%"SCNu8, &page_count) != 1) {
        printf("Bad page number\n");
        usage();
        return -1;
    }

    if (sscanf(argv[4], "%"SCNu32, &buffer_size) != 1) {
        printf("Bad page count\n");
        usage();
        return -1;
    }

    tr.buffer_len = (size_t) buffer_size;
    tr.print_info = 1;

    return run_server(port, page_count, xen_shm_handler_receiver, &tr);
}

int client(int argc, char **argv) {
    int retval;
    int i;
    in_port_t port;
    uint8_t page_count;
    struct in_addr addr;

    uint32_t buffer_size;
    uint32_t client_count;
    uint32_t sleep_interval;

    pthread_t* thread_infos;
    struct xen_shm_handler_data* datas;
    struct xen_shm_handler_transfert* trs;


    if (argc > 8) {
        printf("Too many arguments\n");
        usage();
        return -1;
    }

    if (argc < 8) {
        printf("Not enough arguments\n");
        usage();
        return -1;
    }

    retval = inet_pton(AF_INET, argv[2], &addr);
    if (retval != 1) {
        printf("Bad address\n");
        usage();
        return -1;
    }

    if (sscanf(argv[3], "%"SCNu16, &port) != 1) {
        printf("Bad port\n");
        usage();
        return -1;
    }

    if (sscanf(argv[4], "%"SCNu8, &page_count) != 1) {
        printf("Bad page count\n");
        usage();
        return -1;
    }

    if (sscanf(argv[5], "%"SCNu32, &buffer_size) != 1) {
        printf("Bad buffer size\n");
        usage();
        return -1;
    }

    if (sscanf(argv[6], "%"SCNu32, &client_count) != 1) {
        printf("Bad client count\n");
        usage();
        return -1;
    }

    if (sscanf(argv[7], "%"SCNu32, &sleep_interval) != 1) {
        printf("Bad sleep interval\n");
        usage();
        return -1;
    }

    thread_infos = malloc(sizeof(pthread_t)*client_count);
    datas = malloc(sizeof(struct xen_shm_handler_data)*client_count);
    trs = malloc(sizeof(struct xen_shm_handler_transfert)*client_count);

    if(thread_infos==NULL || datas==NULL || trs==NULL) {
        printf("No memory\n");
        return -1;
    }

    for (i=0; i<(int) client_count; i++) {

        trs[i].buffer_len = (size_t) buffer_size;
        trs[i].print_info = 1;

        datas[i].private_data = &(trs[i]);

        retval = run_client_thread(port, &addr, page_count, xen_shm_handler_sender,  &(datas[i]), &(thread_infos[i]));

        if(retval != 0) {
            perror("run client thread");
            return retval;
        }

        sleep(sleep_interval);

    }

    for (i=0; i<(int)client_count; i++) {
        pthread_join(thread_infos[i], NULL);
    }

    return 0;
}


int main(int argc, char **argv) {
    if(argc < 2) {
        usage();
    }


    if(strcmp(argv[1], "server") == 0) {
        return server(argc, argv);
    } else if(strcmp(argv[1], "client")==0) {
        return client(argc, argv);
    }

    usage();
    return -1;
}

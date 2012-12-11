#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "handler_lib.h"

void* xen_shm_handler_ping_client (struct xen_shm_handler_data* data) {
    struct timespec in_stamp;
    struct timespec out_stamp;
    int i;
    ssize_t len;
    uint8_t noise[PING_PACKET_SIZE];

    while(!data->stop) {
        clock_gettime(CLOCK_REALTIME, &out_stamp);
        for (i = 0; i < PING_SERIES_LENGTH; ++i) {
            len = xen_shm_pipe_write_all(data->send_fd, noise, PING_PACKET_SIZE);
            if (len < 0) {
                printf("Unable to send\n");
                perror("xen_shm_pipe_write_all");
                return -1;
            }
            xen_shm_pipe_flush(data->receive_fd);
            len = xen_shm_pipe_read_all(data->receive_fd, noise, PING_PACKET_SIZE);
            if (len < 0) {
                printf("Unable to receive\n");
                perror("xen_shm_pipe_read_all");
                return -1;
            }
            xen_shm_pipe_flush(data->receive_fd);
        }
        clock_gettime(CLOCK_REALTIME, &in_stamp);
        printf("Sent at %ld.%09ld\n", out_stamp.tv_sec, out_stamp.tv_nsec);
        printf("Received at %ld.%09ld\n", in_stamp.tv_sec, in_stamp.tv_nsec);
    }
    return 0;
}

void* xen_shm_handler_ping_server (struct xen_shm_handler_data* data) {
    uint8_t noise[PING_PACKET_SIZE];
    ssize_t len;

    while (!data->stop) {
        len = xen_shm_pipe_read_all(data->receive_fd, &noise, PING_PACKET_SIZE);
        if (len < 0) {
            printf("Unable to receive\n");
            perror("xen_shm_pipe_read_all");
            return NULL;
        }
        xen_shm_pipe_flush(data->receive_fd);
        len = xen_shm_pipe_write_all(data->send_fd, &noise, PING_PACKET_SIZE);
        if (len < 0) {
            printf("Unable to send\n");
            perror("xen_shm_pipe_write_all");
            return NULL;
        }
        xen_shm_pipe_flush(data->send_fd);
    }
    return NULL;
}

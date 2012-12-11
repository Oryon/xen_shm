#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>

#include "handler_lib.h"

static uint32_t unique_handler_id = 0;

uint32_t xen_shm_handler_get_id(void);

uint32_t
xen_shm_handler_get_id(void) {
    return unique_handler_id++;
}

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
                return NULL;
            }
            xen_shm_pipe_flush(data->receive_fd);
            len = xen_shm_pipe_read_all(data->receive_fd, noise, PING_PACKET_SIZE);
            if (len < 0) {
                printf("Unable to receive\n");
                perror("xen_shm_pipe_read_all");
                return NULL;
            }
            xen_shm_pipe_flush(data->receive_fd);
        }
        clock_gettime(CLOCK_REALTIME, &in_stamp);
        printf("Sent at %ld.%09ld\n", out_stamp.tv_sec, out_stamp.tv_nsec);
        printf("Received at %ld.%09ld\n", in_stamp.tv_sec, in_stamp.tv_nsec);
    }
    return NULL;
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

void* xen_shm_handler_sender(struct xen_shm_handler_data* data) {
    struct xen_shm_handler_transfert* trans_data;
    ssize_t ret;
    struct timeval in_stamp;
    struct timeval out_stamp;
    uint64_t byte_counter;
    uint32_t transfert_id;
    uint64_t delay;
    double bandwidth;


    trans_data = (struct xen_shm_handler_transfert*) data->private_data;
    byte_counter = 0;
    transfert_id = xen_shm_handler_get_id();

    gettimeofday(&in_stamp , NULL);
    while(!data->stop) {
        ret = xen_shm_pipe_write_all(data->send_fd, trans_data->buffer, trans_data->buffer_len);
        if(ret < 0) {
            printf("transfert %"PRIu32" error - ", transfert_id);
            perror("pipe write all");
            return NULL;
        }
        byte_counter+=trans_data->buffer_len;
        if(trans_data->print_info && byte_counter>trans_data->print_interval_bytes) {
            gettimeofday(&out_stamp , NULL);
            delay = ((uint64_t) (out_stamp.tv_sec - in_stamp.tv_sec))*1000000 + (uint64_t) (out_stamp.tv_usec - in_stamp.tv_usec);
            bandwidth = (((double) byte_counter)*8)/((double) delay);

            printf("%"PRIu32", %"PRIu64", %f, Mbps",transfert_id, (uint64_t) out_stamp.tv_sec, bandwidth);
            byte_counter = 0;
            in_stamp = out_stamp;
        }
    }

    return NULL;

}


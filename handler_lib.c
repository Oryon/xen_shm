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
    uint64_t print_threashold;
    uint64_t last_print_s;
    uint32_t transfert_id;
    uint64_t delay;
    uint8_t* buffer;
    double bandwidth;


    trans_data = (struct xen_shm_handler_transfert*) data->private_data;

    buffer = malloc(sizeof(uint8_t)*trans_data->buffer_len);

    transfert_id = xen_shm_handler_get_id();
    byte_counter = 0;
    bandwidth = 2000;
    print_threashold = (bandwidth * 1000000)/4;

    gettimeofday(&in_stamp , NULL);
    last_print_s = in_stamp.tv_sec;

    while(!data->stop) {


        ret = xen_shm_pipe_write_all(data->send_fd, buffer, trans_data->buffer_len);
        if(ret < 0) {
            printf("transfert %"PRIu32" error - ", transfert_id);
            perror("pipe write all");
            return NULL;
        }
        byte_counter+=trans_data->buffer_len;


        if(trans_data->print_info && byte_counter>print_threashold) {

            gettimeofday(&out_stamp , NULL);
            delay = ((uint64_t) (out_stamp.tv_sec - in_stamp.tv_sec))*1000000 + (uint64_t) (out_stamp.tv_usec - in_stamp.tv_usec);
            bandwidth = (((double) byte_counter)*8)/((double) delay);
            print_threashold =(uint64_t)  ((bandwidth * 1000000)/4);

            if(out_stamp.tv_sec > last_print_s) {
                printf("%"PRIu32", %"PRIu64", %"PRIu64", s, %f, Mbps\n",transfert_id,byte_counter ,((uint64_t) out_stamp.tv_sec)*1000 + ((uint64_t) out_stamp.tv_usec)/1000, bandwidth);
                last_print_s = out_stamp.tv_sec;
            }

            byte_counter = 0;
            in_stamp = out_stamp;
        }
    }

    free(buffer);

    return NULL;

}


void* xen_shm_handler_receiver(struct xen_shm_handler_data* data) {
    struct xen_shm_handler_transfert* trans_data;
    ssize_t ret;
    struct timeval in_stamp;
    struct timeval out_stamp;
    uint64_t byte_counter;
    uint64_t print_threashold;
    uint64_t last_print_s;
    uint32_t transfert_id;
    uint64_t delay;
    uint8_t* buffer;
    double bandwidth;


    trans_data = (struct xen_shm_handler_transfert*) data->private_data;

    buffer = malloc(sizeof(uint8_t)*trans_data->buffer_len);

    transfert_id = xen_shm_handler_get_id();
    byte_counter = 0;
    bandwidth = 2000;
    print_threashold = (bandwidth * 1000000)/4;

    gettimeofday(&in_stamp , NULL);
    last_print_s = in_stamp.tv_sec;

    while(!data->stop) {


        ret = xen_shm_pipe_read_all(data->send_fd, buffer, trans_data->buffer_len);
        if(ret < 0) {
            printf("transfert %"PRIu32" error - ", transfert_id);
            perror("pipe write all");
            return NULL;
        }
        byte_counter+=trans_data->buffer_len;


        if(trans_data->print_info && byte_counter>print_threashold) {

            gettimeofday(&out_stamp , NULL);
            delay = ((uint64_t) (out_stamp.tv_sec - in_stamp.tv_sec))*1000000 + (uint64_t) (out_stamp.tv_usec - in_stamp.tv_usec);
            bandwidth = (((double) byte_counter)*8)/((double) delay);
            print_threashold =(uint64_t)  ((bandwidth * 1000000)/4);

            if(out_stamp.tv_sec > last_print_s) {
                printf("%"PRIu32", %"PRIu64", %"PRIu64", r, %f, Mbps\n",transfert_id,byte_counter ,((uint64_t) out_stamp.tv_sec)*1000 + ((uint64_t) out_stamp.tv_usec)/1000, bandwidth);
                last_print_s = out_stamp.tv_sec;
            }

            byte_counter = 0;
            in_stamp = out_stamp;
        }
    }

    free(buffer);

    return NULL;

}




#ifndef HANDLER_LIB_H_
#define HANDLER_LIB_H_

#include <inttypes.h>

#include "xen_shm_pipe.h"

struct xen_shm_handler_data {
  xen_shm_pipe_p receive_fd;
  xen_shm_pipe_p send_fd;
  volatile int stop;
  void *private_data;
};

typedef void* (*handler_run) (struct xen_shm_handler_data* data);


#define PING_PACKET_SIZE 10
#define PING_SERIES_LENGTH  500

void* xen_shm_handler_ping_client (struct xen_shm_handler_data* data);

void* xen_shm_handler_ping_server (struct xen_shm_handler_data* data);


struct xen_shm_handler_transfert {
    size_t buffer_len;
    uint8_t print_info;
};

void* xen_shm_handler_sender(struct xen_shm_handler_data* data);

void* xen_shm_handler_receiver(struct xen_shm_handler_data* data);


#endif /* HANDLER_LIB_H_ */

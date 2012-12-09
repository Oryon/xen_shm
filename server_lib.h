/*
 * Simplified interface to construct a server for bidirectionnal transfer using xen_shm_pipe
 */

#ifndef __XEN_SHM_SERVER_LIB_H__
#define __XEN_SHM_SERVER_LIB_H__

#include "xen_shm_pipe.h"


struct xen_shm_server_data {
  xen_shm_pipe_p receive_fd;
  xen_shm_pipe_p send_fd;
  volatile int stop;
  void *private_data;
};

typedef void* (*listener_init) (struct xen_shm_server_data* data);

int run_server(int port, uint8_t proposed_page_page_count, listener_init initializer, void *private_data);

#endif /* __XEN_SHM_SERVER_LIB_H__ */



























































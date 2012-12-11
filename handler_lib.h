
#ifndef HANDLER_LIB_H_
#define HANDLER_LIB_H_

#include "xen_shm_pipe.h"

struct xen_shm_handler_data {
  xen_shm_pipe_p receive_fd;
  xen_shm_pipe_p send_fd;
  volatile int stop;
  void *private_data;
};

typedef void* (*handler_run) (struct xen_shm_handler_data* data);


#endif /* HANDLER_LIB_H_ */

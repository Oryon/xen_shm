/*
 * Simplified interface to construct a server for bidirectionnal transfer using xen_shm_pipe
 */

#ifndef __XEN_SHM_CLIENT_LIB_H__
#define __XEN_SHM_CLIENT_LIB_H__

#include <netinet/in.h>

#include "xen_shm_pipe.h"
#include "handler_lib.h"

int init_pipe(in_port_t distant_port, struct in_addr *distant_addr, xen_shm_pipe_p *receive_fd, xen_shm_pipe_p *send_fd, uint8_t proposed_page_page_count);

//Starts a handler in a new thread (given in thread_info)
int run_client_thread(in_port_t distant_por, struct in_addr *distant_addr, uint8_t proposed_page_page_count,
        handler_run handler_fct,  struct xen_shm_handler_data* hdlr_data, pthread_t* thread_info);

//Runs the handler (and give the returned value in returned_value)
int run_client(in_port_t distant_por, struct in_addr *distant_addr, uint8_t proposed_page_page_count,
        handler_run handler_fct,  struct xen_shm_handler_data* hdlr_data, void** returned_value);

#endif /* __XEN_SHM_SERVER_LIB_H__ */



























































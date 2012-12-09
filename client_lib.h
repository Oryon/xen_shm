/*
 * Simplified interface to construct a server for bidirectionnal transfer using xen_shm_pipe
 */

#ifndef __XEN_SHM_CLIENT_LIB_H__
#define __XEN_SHM_CLIENT_LIB_H__

#include "xen_shm_pipe.h"
#include <netinet/in.h>

int init_pipe(in_port_t distant_port, struct in_addr *distant_addr, xen_shm_pipe_p *receive_fd, xen_shm_pipe_p *send_fd, uint8_t proposed_page_page_count);

#endif /* __XEN_SHM_SERVER_LIB_H__ */



























































/*
 * Simplified interface to construct a server for bidirectionnal transfer using xen_shm_pipe
 */

#ifndef __XEN_SHM_SERVER_LIB_H__
#define __XEN_SHM_SERVER_LIB_H__

#include "xen_shm_pipe.h"
#include "handler_lib.h"



int run_server(int port, uint8_t proposed_page_page_count, handler_run initializer, void *private_data);

#endif /* __XEN_SHM_SERVER_LIB_H__ */



























































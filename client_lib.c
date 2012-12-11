#include "client_lib.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>

#include "xen_shm_udp_proto.h"

#define CLIENT_LIB_DEBUG 0
#if CLIENT_LIB_DEBUG
# define PRINTF(...)  printf(__VA_ARGS__)
#else /* !CLIENT_LIB_DEBUG */
# define PRINTF(...)
#endif /* ?CLIENT_LIB_DEBUG */


int
init_pipe(in_port_t distant_port, struct in_addr *distant_addr, xen_shm_pipe_p *receive_fd, xen_shm_pipe_p *send_fd, uint8_t proposed_page_page_count)
{
    struct sockaddr_in addr;
    int ret;
    int client_fd;
    int return_value;
    ssize_t len;
    uint8_t buffer[XEN_SHM_UDP_PROTO_MAX_LEN];
    struct xen_shm_udp_proto_header *header;
    struct xen_shm_udp_proto_client_hello *client_hello;
    struct xen_shm_udp_proto_grant *grant;
    //enum xen_shm_pipe_conv distant_convention;

    return_value = -1;

    client_fd = socket(PF_INET, SOCK_DGRAM, 0);

    if (client_fd < 0) {
        printf("Unable to open an udp socket");
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_port = htons(distant_port);
    memcpy(&addr.sin_addr, distant_addr,sizeof(struct in_addr));

    memset(buffer, 0, XEN_SHM_UDP_PROTO_MAX_LEN);
    header = (struct xen_shm_udp_proto_header*) buffer;
    client_hello = (struct xen_shm_udp_proto_client_hello*) buffer;
    grant = (struct xen_shm_udp_proto_grant*) buffer;

    ret = xen_shm_pipe_init(receive_fd, xen_shm_pipe_mod_read, xen_shm_pipe_conv_reader_offers);
    if (ret != 0) {
        printf("Unable to init xen_shm_pipe\n");
        perror("xen_shm_pipe_init");
        goto shutdown_socket;
    }

    ret = xen_shm_pipe_init(send_fd, xen_shm_pipe_mod_write, xen_shm_pipe_conv_reader_offers);
    if (ret != 0) {
        printf("Unable to init xen_shm_pipe\n");
        perror("xen_shm_pipe_init");
        goto clean_receive_fd;
    }

    header->version = XEN_SHM_UDP_PROTO_VERSION;
    header->message = XEN_SHM_UDP_PROTO_CLIENT_HELLO;
    ret = xen_shm_pipe_getdomid(*send_fd, &client_hello->domid);
    if (ret != 0) {
        printf("Unable to get local domid\n");
        perror("xen_shm_pipe_getdomid");
        goto clean_send_fd;
    }

    len = sendto(client_fd, buffer, sizeof(struct xen_shm_udp_proto_client_hello), /* No flag */ 0, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if (len < 0) {
        printf("Unable to send message\n");
        perror("sendto");
        goto cancel_server;
    }

    if ((size_t)len != sizeof(struct xen_shm_udp_proto_client_hello)) {
        printf("Unable to send message (bad size=\n");
        perror("sendto");
        goto cancel_server;
    }

    len = recv(client_fd, buffer, XEN_SHM_UDP_PROTO_MAX_LEN, 0 /* No flag */);
    if (len <= 0) {
        printf("Unable to receive message\n");
        perror("recv");
        goto cancel_server;
    }

    if (header->message == XEN_SHM_UDP_PROTO_SERVER_RESET) {
        printf("Server error\n");
        goto cancel_server;
    }

    if (header->message != XEN_SHM_UDP_PROTO_SERVER_GRANT) {
        printf("Protocol error\n");
        goto cancel_server;
    }

    if ((size_t)len < sizeof(struct xen_shm_udp_proto_grant)) {
        printf("Bad packet: too short");
        goto cancel_server;
    }

    switch (grant->mode) {
        case XEN_SHM_UDP_PROTO_GRANT_MODE_WRITER_OFFERER:
            printf("xen_shm_pipe_conv_writer_offers not supporter\n");
            goto cancel_server;
        case XEN_SHM_UDP_PROTO_GRANT_MODE_READER_OFFERER:
            //distant_convention = xen_shm_pipe_conv_reader_offers;
            break;
        default:
            printf("Protocol error: bad mode\n");
            goto cancel_server;
    }

    PRINTF("Mapping grant from %"PRIu32": first=%"PRIu32"\n", grant->domid, grant->grant_ref);
    ret = xen_shm_pipe_connect(*send_fd, grant->page_count, grant->domid, grant->grant_ref);
    if (ret != 0) {
        printf("Unable to connect xen_shm_pipe\n");
        perror("xen_shm_pipe_connect");
        goto cancel_server;
    }

    PRINTF("New grant for %"PRIu32":", grant->domid);
    ret = xen_shm_pipe_offers(*receive_fd, proposed_page_page_count, grant->domid, &grant->domid, &grant->grant_ref);
    PRINTF("(from %"PRIu32"): first=%"PRIu32"\n", grant->domid, grant->grant_ref);
    if (ret != 0) {
        printf("Unable to offer xen_shm_pipe\n");
        perror("xen_shm_pipe_offers");
        goto cancel_server;
    }

    header->message = XEN_SHM_UDP_PROTO_CLIENT_GRANT;
    grant->page_count = proposed_page_page_count;

    len = sendto(client_fd, buffer, sizeof(struct xen_shm_udp_proto_grant), /* No flag */ 0, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if (len < 0) {
        printf("Unable to send message\n");
        perror("sendto");
        goto cancel_server;
    }

    if ((size_t)len != sizeof(struct xen_shm_udp_proto_grant)) {
        printf("Unable to send message (bad size=\n");
        perror("sendto");
        goto cancel_server;
    }

    shutdown(client_fd, SHUT_RDWR);
    return 0;


cancel_server:
    header->message = XEN_SHM_UDP_PROTO_CLIENT_RESET;
    sendto(client_fd, buffer, sizeof(struct xen_shm_udp_proto_header), /* No flag */ 0, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));

clean_send_fd:
    xen_shm_pipe_free(*send_fd);

clean_receive_fd:
    xen_shm_pipe_free(*receive_fd);

shutdown_socket:
    shutdown(client_fd, SHUT_RDWR);

    return return_value;
}



int
run_client_thread(in_port_t distant_por, struct in_addr *distant_addr, uint8_t proposed_page_page_count,
        handler_run handler_fct,  struct xen_shm_handler_data* hdlr_data, pthread_t* thread_info)
{

    int ret;

    hdlr_data->stop = 0;

    ret = init_pipe(distant_por, distant_addr, &hdlr_data->receive_fd, &hdlr_data->send_fd, proposed_page_page_count);
    if(ret!=0) {
        perror("init pipe");
        return ret;
    }

    ret = pthread_create(thread_info, /* Default attr */ NULL, (void * (*)(void *))handler_fct, hdlr_data);
    if (ret != 0) {
        perror("pthread create");
        xen_shm_pipe_free(hdlr_data->receive_fd);
        xen_shm_pipe_free(hdlr_data->send_fd);
        return ret;
    }

    return 0;
}


int
run_client(in_port_t distant_por, struct in_addr *distant_addr, uint8_t proposed_page_page_count,
        handler_run handler_fct,  struct xen_shm_handler_data* hdlr_data, void** returned_value)
{
    int ret;

    hdlr_data->stop = 0;

    ret = init_pipe(distant_por, distant_addr, &hdlr_data->receive_fd, &hdlr_data->send_fd, proposed_page_page_count);
    if(ret!=0) {
        perror("init pipe");
        return ret;
    }

    *returned_value = handler_fct(hdlr_data);

    return 0;
}


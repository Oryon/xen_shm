#include "server_lib.h"

#include <ev.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "xen_shm_udp_proto.h"

struct pthread_list {
    pthread_t child;
    struct pthread_list *next;
};

struct opening_list {
    in_port_t distant_port;
    struct in_addr distant_addr;
    xen_shm_pipe_p send_fd;
    struct opening_list *next;
};

struct internal_data {
    uint8_t proposed_page_page_count;
    void *private_data;
    listener_init initializer;
    struct pthread_list *childs;
    struct opening_list *current;
};

struct ev_loop *event_loop;
struct ev_timer* event_killer;

static void
event_end(struct ev_loop *loop, struct ev_timer *w, int revents) {
    ev_unloop(loop, EVUNLOOP_ALL);
}

static void
clean(int sig)
{
    /* Stop the listener */
    ev_timer_set(event_killer, 0, 0);
    ev_timer_start(event_loop, event_killer);
}

static struct opening_list*
find_opening(struct internal_data *data, struct sockaddr_in *source)
{
    struct opening_list *temp;
    temp = data->current;
    while (temp != NULL) {
        if ((temp->distant_port == source->sin_port)
            && (memcmp(&source->sin_addr, &temp->distant_addr, sizeof(struct in_addr)) == 0)) {
            break;
        }
    }
    return temp;
}

static void
free_opening(struct internal_data *data, struct opening_list* o)
{
    struct opening_list *temp = data->current;

    if (temp == NULL) {
        return;
    }

    while (temp->next != NULL) {
        if (temp->next == o) {
            temp->next = o->next;
            break;
        }
    }

    free(o);
}


static void
udp_readable_cb(struct ev_loop *loop, struct ev_io *w, int revents)
{
    int ret;
    ssize_t len;
    size_t send_len;
    struct sockaddr_in source;
    socklen_t addr_len;
    uint8_t buffer[XEN_SHM_UDP_PROTO_MAX_LEN];
    struct internal_data *data;
    struct pthread_list *c_new;
    struct opening_list *o_new;
    struct xen_shm_udp_proto_header *header;
    struct xen_shm_udp_proto_client_hello *client_hello;
    struct xen_shm_udp_proto_grant *grant;
    struct xen_shm_server_data *client_data;

    data = w->data;
    send_len = 0;

    if (revents & (int)EV_ERROR) {
        printf("UDP error\n");
        clean(0);
        return;
    }

    if (revents & EV_READ) {
        len = recvfrom(w->fd, buffer, XEN_SHM_UDP_PROTO_MAX_LEN, 0 /* No flag */, (struct sockaddr *) &source, &addr_len);
        if (len < 0) {
            perror("recvfrom error");
            return;
        }
        if (addr_len != sizeof(struct sockaddr_in)) {
            printf("Addr size mismatch, potential buffer overflow, failing\n");
            exit(1);
        }
        if ((size_t) len < sizeof(struct xen_shm_udp_proto_header)) {
            printf("Packet too short (or error), ignoring\n");
            return;
        }
        header = (struct xen_shm_udp_proto_header*)buffer;
        if (header->version != XEN_SHM_UDP_PROTO_VERSION) {
            printf("Version missmatch, ignoring\n");
            return;
        }
        switch(header->message) {
            case XEN_SHM_UDP_PROTO_CLIENT_HELLO:
                client_hello = (struct xen_shm_udp_proto_client_hello*) buffer;
                o_new = calloc(1, sizeof(struct opening_list));
                if (o_new == NULL) {
                    printf("Calloc error !\n");
                    return;
                }
                ret = xen_shm_pipe_init(&o_new->send_fd, xen_shm_pipe_mod_write, xen_shm_pipe_conv_writer_offers);
                if (ret == 0) {
                    printf("Unable to init xen_shm_pipe\n");
                    goto server_reset;
                }
                grant = (struct xen_shm_udp_proto_grant*)buffer;
                ret = xen_shm_pipe_offers(o_new->send_fd, data->proposed_page_page_count, client_hello->domid, &grant->domid, &grant->grant_ref);
                if (ret == 0) {
                    printf("Unable to init xen_shm_pipe in offerer mode \n");
                    xen_shm_pipe_free(o_new->send_fd);
                    goto server_reset;
                }
                o_new->distant_port = source.sin_port;
                memcpy(&o_new->distant_addr, (struct sockaddr *) &source, addr_len);
                grant->header.message = XEN_SHM_UDP_PROTO_SERVER_GRANT;
                grant->mode = XEN_SHM_UDP_PROTO_GRANT_MODE_WRITER_OFFERER;
                grant->page_count = data->proposed_page_page_count;
                send_len = sizeof(struct xen_shm_udp_proto_grant);
                o_new->next = data->current;
                data->current = o_new;
                goto send;
                break;
            case XEN_SHM_UDP_PROTO_SERVER_RESET:
            case XEN_SHM_UDP_PROTO_SERVER_GRANT:
                /* O_o ignoring */
                return;
            case XEN_SHM_UDP_PROTO_CLIENT_RESET:
                o_new = find_opening(data, &source);
                if (o_new != NULL) {
                    goto free_o_new;
                }
                return;
            case XEN_SHM_UDP_PROTO_CLIENT_GRANT:
                o_new = find_opening(data, &source);
                if (o_new == NULL) {
                    goto server_reset;
                }
                grant = (struct xen_shm_udp_proto_grant*)buffer;
                client_data = calloc(1, sizeof(struct xen_shm_server_data));
                if (client_data == NULL) {
                    printf("Calloc error !\n");
                    goto free_o_new;
                }
                c_new = calloc(1, sizeof(struct pthread_list));
                if (c_new == NULL) {
                    printf("Calloc error !\n");
                    free(client_data);
                    goto free_o_new;
                }
                ret = xen_shm_pipe_init(&client_data->receive_fd, xen_shm_pipe_mod_read, xen_shm_pipe_conv_writer_offers);
                if (ret == 0) {
                    printf("Unable to init xen_shm_pipe\n");
                    goto free_o_new;
                }
                ret = xen_shm_pipe_connect(client_data->receive_fd, grant->page_count, grant->domid, grant->grant_ref);
                if (ret == 0) {
                    printf("Unable to init xen_shm_receiver\n");
                    xen_shm_pipe_free(client_data->receive_fd);
                    goto free_o_new;
                }
                client_data->send_fd = o_new->send_fd;
                client_data->private_data = data->private_data;
                ret = pthread_create(&c_new->child, /* Default attr */ NULL, (void * (*)(void *))data->initializer, client_data);
                if (ret != 0) {
                    xen_shm_pipe_free(client_data->receive_fd);
                    xen_shm_pipe_free(client_data->send_fd);
                    free_opening(data, o_new);
                    free(client_data);
                    free(c_new);
                    return;
                }
                c_new->next = data->childs;
                data->childs = c_new;
                return;
            default:
                printf("Bad packet message, ignoring\n");
                return;
        }
    }
    return;

free_o_new:
    xen_shm_pipe_free(o_new);
    free_opening(data, o_new);

server_reset:
    header->message = XEN_SHM_UDP_PROTO_SERVER_RESET;
    send_len = sizeof(struct xen_shm_udp_proto_header);

send:
    sendto(w->fd, buffer, send_len, /* No flag */ 0, (struct sockaddr *) &source, addr_len);
}



int
run_server(int port, uint8_t proposed_page_page_count, listener_init initializer, void *private_data)
{
    struct internal_data *data;
    struct ev_io *event;
    struct sockaddr_in binding_addr;
    struct pthread_list   *c_it,
                        *c_next;
    struct opening_list   *o_it,
                        *o_next;
    int server_fd;

    event_killer = calloc(1, sizeof(struct ev_timer));
    if (event_killer != NULL) {
        printf("Arg, no memory !\n");
        return -1;
    }

    event = calloc(1, sizeof(struct ev_io));
    if (event != NULL) {
        printf("Arg, no memory !\n");
        free(event_killer);
        return -1;
    }

    data = calloc(1, sizeof(struct internal_data));
    if (data == NULL) {
        printf("Arg, no memory !\n");
        free(event_killer);
        free(event);
        return -1;
    }
    data->private_data = private_data;
    data->initializer = initializer;
    data->proposed_page_page_count = proposed_page_page_count;
    data->childs = NULL;
    data->current = NULL;

    server_fd = socket(PF_INET, SOCK_DGRAM, 0);

    if (server_fd < 0) {
        printf("Unable to open an udp socket");
        perror("socket");
        goto free_data;
    }

    memset(&binding_addr, 0, sizeof(struct sockaddr_in));
    binding_addr.sin_port= htons(port);
    binding_addr.sin_family = AF_INET;

    if (bind(server_fd, (struct sockaddr*)&binding_addr, sizeof(struct sockaddr_in)) < 0) {
        printf("Unable to bick socket\n");
        perror("bind");
        goto close_fd;
    }

    ev_io_init(event, udp_readable_cb, server_fd, EV_READ);
    event->data = data;
    ev_io_start(event_loop, event);

    ev_init(event_killer, event_end);

    signal(SIGINT, clean);

    ev_loop(event_loop, 0);

    /* Close the currently half-opened connections */
    o_it = data->current;
    while (o_it != NULL) {
        if (o_it->send_fd != NULL) {
            xen_shm_pipe_free(o_it->send_fd);
        }
        o_next = o_it->next;
        free(o_it);
        o_it = o_next;
    }

    /* Send SIGINT signal to childs */
    c_it = data->childs;
    while (c_it != NULL) {
        pthread_kill(c_it->child, SIGINT);
    }

    /* Wait for childs end*/
    c_it = data->childs;
    while (c_it != NULL) {
        pthread_join(c_it->child, NULL);
    }

    /* Free the list */
    c_it = data->childs;
    while (c_it != NULL) {
        c_next = c_it->next;
        free(c_it);
        c_it = c_next;
    }

close_fd:
    shutdown(server_fd, SHUT_RDWR);

free_data:
    free(data);
    free(event);
    free(event_killer);

    return 0;
}

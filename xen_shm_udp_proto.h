#ifndef __XEN_SHM_UDP_PROTO_H__
#define __XEN_SHM_UDP_PROTO_H__

#define XEN_SHM_UDP_PROTO_VERSION 0x01
#define XEN_SHM_UDP_PROTO_MAX_LEN 40


#define XEN_SHM_UDP_PROTO_CLIENT_HELLO 0x01
#define XEN_SHM_UDP_PROTO_SERVER_RESET 0x02

#define XEN_SHM_UDP_PROTO_SERVER_GRANT 0x03
#define XEN_SHM_UDP_PROTO_CLIENT_RESET 0x04

#define XEN_SHM_UDP_PROTO_CLIENT_GRANT 0x05

struct xen_shm_udp_proto_header {
    uint8_t  version;
    uint8_t  message;
    uint16_t reserved;
    uint32_t big_reserved;
} __attribute__ ((__packed__));

struct xen_shm_udp_proto_client_hello {
    struct xen_shm_udp_proto_header header;
    uint32_t domid;
} __attribute__ ((__packed__));

#define XEN_SHM_UDP_PROTO_GRANT_MODE_WRITER_OFFERER 0x01
#define XEN_SHM_UDP_PROTO_GRANT_MODE_READER_OFFERER 0x02

struct xen_shm_udp_proto_grant {
    struct xen_shm_udp_proto_header header;
    uint32_t grant_ref;
    uint32_t domid;
    uint8_t  mode;
    uint8_t  page_count;
} __attribute__ ((__packed__));

#endif /* __XEN_SHM_UDP_PROTO_H__ */

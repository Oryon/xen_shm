/*
 * Waits signals froms the shared memory
 */


#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../xen_shm.h"


static const char default_device_name[] = "/dev/xen_shm";

static int fd;



int
main(int argc, char *argv[])
{
    int retval;
    domid_t target;
    struct xen_shm_ioctlarg_offerer init_offerer;

    if (argc > 2) {
        printf("Too many arguments\n");
        return -1;
    }
    if (argc == 2) {
        if(sscanf(argv[1], "%"SCNu16, &target) != 1) {
            printf("Bad argument, need to be a domid_t\n");
        }
    } else {
        printf("No destination domid set, defaulting to domid_self\n");
        target = DOMID_SELF;
    }

    fd = open(default_device_name, O_RDWR);
    if (fd < 0) {
        perror("open device");
        return -1;
    }

    init_offerer.pages_count = 1;
    init_offerer.dist_domid = target;

    retval = ioctl(fd, XEN_SHM_IOCTL_INIT_OFFERER, &init_offerer);
    if (retval != 0) {
        perror("ioctl init offerer");
    } else {
        printf("Offerer initialisation success: %"PRIu32"\n", init_offerer.grant);
    }

    while(1) {
        printf("Waiting for a signal\n");
        retval = ioctl(fd, XEN_SHM_IOCTL_WAIT, 0);
        if (retval != 0) {
            perror("ioctl error");
            return -1;
        } else {
            printf("Signal received\n");
        }
    }

    return 0;
}

/*
 * Regularly notifies through the shared memory
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
    struct xen_shm_ioctlarg_receiver init_receiver;

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

    while (scanf("%"SCNu32, &init_receiver.grant) != 1);

    init_receiver.pages_count = 1;
    init_receiver.dist_domid = DOMID_SELF;

    retval = ioctl(fd, XEN_SHM_IOCTL_INIT_RECEIVER, &init_receiver);
    if (retval != 0) {
        perror("ioctl init receiver");
    } else {
        printf("Receiver initialisation success!\n");
    }



    while(1) {
        sleep((unsigned int) (1+rand()%10));
        retval = ioctl(fd, XEN_SHM_IOCTL_SSIG, 0);
        if (retval != 0) {
            perror("ioctl error");
            return -1;
        } else {
            printf("Signal sent\n");
        }

    }

    return 0;
}

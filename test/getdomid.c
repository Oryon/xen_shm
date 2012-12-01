#include <fcntl.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>

#include "../xen_shm.h"

static const char default_device_name[] = "/dev/xen_shm";

int
main(int argc, char *argv[])
{
    const char* device_name;
    int fd, retval;
    struct xen_shm_ioctlarg_getdomid getdomid;

    if (argc > 2) {
        printf("Too many arguments\n");
        return -1;
    }
    if (argc == 2) {
        device_name = argv[1];
    } else {
        printf("No device given, using default : %s\n", default_device_name);
        device_name = default_device_name;
    }

    fd = open(device_name, O_RDWR);
    if (fd < 0) {
        perror("open device");
        return -1;
    }

    retval = ioctl(fd, XEN_SHM_IOCTL_GET_DOMID, &getdomid);
    if (retval != 0) {
        perror("ioctl getdomid");
    } else {
        printf("Get domid success: %"PRIu16"\n", getdomid.local_domid);
    }

    close(fd);
    return 0;
}

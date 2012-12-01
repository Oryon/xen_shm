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

static int *mapped_addr;
static int fd;

static void
clean(int sig)
{
    /* Unmap */
    if (munmap(mapped_addr, sizeof(unsigned int) * 5) != 0) {
        perror("Unable to unmap");
    }
    close(fd);
    exit(0);
}

int
main(int argc, char *argv[])
{
    const char* device_name;
    int retval;
    struct xen_shm_ioctlarg_offerer init_offerer;

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

    init_offerer.pages_count = 1;
    init_offerer.dist_domid = DOMID_SELF;

    retval = ioctl(fd, XEN_SHM_IOCTL_INIT_OFFERER, &init_offerer);
    if (retval != 0) {
        perror("ioctl init offerer");
    } else {
        printf("Offerer initialisation success: %"PRIu32"\n", init_offerer.grant);
    }

    sleep(20);

    mapped_addr = mmap(0, sizeof(unsigned int) * 5, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    if (mapped_addr == MAP_FAILED) {
        perror("Unabme to map");
        close(fd);
        exit(-1);
    }
    printf("Mmap success (%p)\n", mapped_addr);

    signal(SIGINT, clean);

    while(1) {
        sleep(10);
        printf("Offerrer read %d, %d\n", mapped_addr[0], mapped_addr[1]);
        mapped_addr[2] = rand();
        mapped_addr[3] = rand();
        printf("Offerrer set %d, %d\n", mapped_addr[2], mapped_addr[3]);
    }

    return 0;
}

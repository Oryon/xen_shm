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
        printf("Receiver: No destination domid set, defaulting to domid_self\n");
        target = DOMID_SELF;
    }

    fd = open(default_device_name, O_RDWR);
    if (fd < 0) {
        perror("Receiver: open device");
        return -1;
    }

    while (scanf("%"SCNu32, &init_receiver.grant) != 1);

    init_receiver.pages_count = 1;
    init_receiver.dist_domid = DOMID_SELF;

    retval = ioctl(fd, XEN_SHM_IOCTL_INIT_RECEIVER, &init_receiver);
    if (retval != 0) {
        perror("Receiver: ioctl init");
    } else {
        printf("Receiver: initialisation success!\n");
    }

    sleep(2);

    mapped_addr = mmap(0, sizeof(unsigned int) * 5, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    if (mapped_addr == MAP_FAILED) {
        perror("Receiver: unable to map");
        close(fd);
        exit(-1);
    }
    printf("Receiver: mmap success (%p)\n", mapped_addr);

    signal(SIGINT, clean);

    srand(1);

    while(1) {
        sleep(5);
        printf("Receiver read %d, %d\n", mapped_addr[2], mapped_addr[3]);
        mapped_addr[0] = rand();
        mapped_addr[1] = rand();
        printf("Receiver set %d, %d\n", mapped_addr[0], mapped_addr[1]);
    }

    return 0;
}

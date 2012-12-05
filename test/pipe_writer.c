#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "../xen_shm_pipe.h"


/*
 * Convention writer_offers
 * This is the RECEIVER
 */

#define PAGE_COUNT 1

int main(int argc, char **argv) {
    int retval;
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;

    xen_shm_pipe_p pipe;

    printf("Pipe writer now starting\n");

    if((retval = xen_shm_pipe_init(&pipe, write, writer_offers))) {
        printf("Pipe init error %"PRIu32"\n", retval);
        return -1;
    }


    printf("Distant domain id: ");
    if((scanf("%"SCNu32, &dist_domid)!=1)) {
        printf("Scanf error");
        return -1;
    }


    if((retval = xen_shm_pipe_offers(pipe, PAGE_COUNT, dist_domid, &local_domid, &grant_ref))) {
        printf("Pipe get domid error %"PRIu32"\n", retval);
        return -1;
    }

    printf("Local domain id: %"PRIu32"\n", local_domid);
    printf("Grant reference id: %"PRIu32"\n", grant_ref);

    printf("Will now wait for at most 30 seconds\n");
    if((retval = xen_shm_pipe_wait(pipe, 30*1000))) {
        printf("Pipe wait error %"PRIu32"\n", retval);
        return -1;
    }

    printf("Connected successfully !\n");
    sleep(5);

    printf("I will now close the pipe\n");
    xen_shm_pipe_free(pipe);

}


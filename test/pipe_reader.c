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

    printf("Pipe reader now starting\n");

    if((retval = xen_shm_pipe_init(&pipe, read, writer_offers))) {
        printf("Pipe init error %"PRIu32"\n", retval);
        return -1;
    }

    if((retval = xen_shm_pipe_getdomid(pipe, &local_domid))) {
        printf("Pipe get domid error %"PRIu32"\n", retval);
        return -1;
    }

    printf("RECEIVER domain id: "PRIu32"\n", local_domid);
    printf("Distant domain id: ");
    if((scanf("%"SCNu32, &dist_domid)!=1)) {
        printf("Scanf error");
        return -1;
    }

    printf("Offerer's grant ref: ");
    if((scanf("%"SCNu32, &grant_ref)!=1)) {
        printf("Scanf error");
        return -1;
    }



    if((retval = xen_shm_pipe_connect(pipe,PAGE_COUNT,dist_domid, grant_ref))) {
        printf("Pipe connect error %"PRIu32"\n", retval);
        return -1;
    }

    printf("Connected successfully !\n");
    sleep(5);

    printf("I will now close the pipe\n");
    xen_shm_pipe_free(pipe);

}


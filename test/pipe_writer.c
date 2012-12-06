#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "../xen_shm_pipe.h"


/*
 * Convention writer_offers
 * This is the OFFERER
 */

#define PAGE_COUNT 1

int main(int argc, char **argv) {
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;

    xen_shm_pipe_p pipe;

    printf("Pipe writer now starting\n");

    if(xen_shm_pipe_init(&pipe, xen_shm_pipe_mod_write, xen_shm_pipe_conv_writer_offers)) {
        perror("Pipe init");
        return -1;
    }


    printf("Distant domain id: ");
    if((scanf("%"SCNu32, &dist_domid)!=1)) {
        printf("Scanf error");
        return -1;
    }


    if(xen_shm_pipe_offers(pipe, PAGE_COUNT, dist_domid, &local_domid, &grant_ref)) {
        perror("Pipe get domid");
        return -1;
    }

    printf("Local domain id: %"PRIu32"\n", local_domid);
    printf("Grant reference id: %"PRIu32"\n", grant_ref);

    printf("Will now wait for at most 30 seconds\n");
    if(xen_shm_pipe_wait(pipe, 30*1000)) {
        perror("Pipe wait");
        return -1;
    }

    printf("Connected successfully !\n");
    sleep(5);

    printf("I will now close the pipe\n");
    xen_shm_pipe_free(pipe);

    return 0;

}


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
#define BUFFER_SIZE 128


int main(int argc, char **argv) {
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;

    ssize_t retval;
    uint8_t buffer[BUFFER_SIZE];

    xen_shm_pipe_p pipe;

    printf("Pipe reader now starting\n");

    if(xen_shm_pipe_init(&pipe, xen_shm_pipe_mod_read, xen_shm_pipe_conv_writer_offers)) {
        perror("Pipe init");
        return -1;
    }

    if(xen_shm_pipe_getdomid(pipe, &local_domid)) {
        perror("Pipe get domid");
        return -1;
    }

    printf("RECEIVER domain id: %"PRIu32"\n", local_domid);
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



    if(xen_shm_pipe_connect(pipe,PAGE_COUNT,dist_domid, grant_ref)) {
        perror("Pipe connect");
        return -1;
    }

    printf("Connected successfully !\n");
    sleep(5);

    printf("I will now read what is incoming\n");
    while((retval = xen_shm_pipe_read(pipe,buffer,BUFFER_SIZE-1))>0) {
        buffer[retval] = '\0';
        printf("%s", buffer);
    }

    if(retval == 0) {
        printf("End of file\n");
    } else {
        perror("xen pipe read");
        return -1;
    }

    printf("I will now close the pipe\n");
    xen_shm_pipe_free(pipe);

    return 0;
}


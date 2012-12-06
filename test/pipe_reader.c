#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>

#include "../xen_shm_pipe.h"


/*
 * Convention writer_offers
 * This is the RECEIVER
 */

#define PAGE_COUNT 1
#define BUFFER_SIZE 512


static uint32_t checksum;
static uint64_t rcv_bytes;
static xen_shm_pipe_p xpipe;

static void
clean(int sig)
{
    printf("\n");
    if(sig >= 0) {
        printf("Signal received: %i\n", sig);
    }
    printf("Now closing the pipe\n");
    xen_shm_pipe_free(xpipe);

    printf("%"PRIu64" bytes received \n", rcv_bytes);
    printf("check sum: %"PRIu32"  \n", checksum);

    exit(0);
}


int main(int argc, char **argv) {
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;

    ssize_t retval;
    uint8_t buffer[BUFFER_SIZE];

    uint32_t to_read;
    int i;

    printf("Pipe reader now starting\n");

    if(xen_shm_pipe_init(&xpipe, xen_shm_pipe_mod_read, xen_shm_pipe_conv_writer_offers)) {
        perror("Pipe init");
        return -1;
    }

    if(xen_shm_pipe_getdomid(xpipe, &local_domid)) {
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



    if(xen_shm_pipe_connect(xpipe,PAGE_COUNT,dist_domid, grant_ref)) {
        perror("Pipe connect");
        return -1;
    }

    printf("Connected successfully !\n");
    signal(SIGINT, clean);


    rcv_bytes = 0;
    checksum = 0;
    while(1) {
        printf("\nHow many bytes shall I read ? ");
        if((scanf("%"SCNu32, &to_read)!=1)) {
            printf("Scanf error");
            return -1;
        }

        while(to_read) {
            retval = xen_shm_pipe_read(xpipe,buffer,(BUFFER_SIZE-1>to_read)?to_read:(BUFFER_SIZE-1));

            if(retval == 0) {
                printf("End of file\n");
                clean(0);
                return 0;
            } else if(retval < 0){
                perror("xen pipe read");
                clean(0);
                return 0;
            }

            to_read -= (uint32_t) retval;
            rcv_bytes += (uint64_t) retval;
            for(i=0; i<retval; ++i) {
                checksum = checksum + ((uint32_t) buffer[i] + 10)*((uint32_t) buffer[i] + 20);
                //printf("Checksum with '%"PRIu8"' -- %"PRIu32"\n", buffer[i], checksum);
            }
            printf("\r%"PRIu64, rcv_bytes);
            //buffer[retval]='\0';
            //printf("%s", buffer);
            fflush(stdout);
        }


    }


    clean(0);

    return 0;
}


#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "../xen_shm_pipe.h"


/*
 * Convention writer_offers
 * This is the OFFERER
 */

#define PAGE_COUNT 10
#define BUFFER_SIZE 5000

static uint32_t checksum;
static uint64_t sent_bytes;
static xen_shm_pipe_p xpipe;

static void
clean(int sig)
{
    printf("\n");
    if(sig > 0) {
        printf("Signal received: %i\n", sig);
    }
    printf("Now closing the pipe\n");
    xen_shm_pipe_free(xpipe);

    printf("%"PRIu64" bytes sent \n", sent_bytes);
    printf("check sum: %"PRIu32"  \n", checksum);

    exit(0);
}


int main(int argc, char **argv) {
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;


    int i;
    uint8_t buffer[BUFFER_SIZE];
    size_t offset;
    size_t msg_len;
    ssize_t retval;
    ssize_t stdin_read;




    printf("Pipe writer now starting\n");

    if(xen_shm_pipe_init(&xpipe, xen_shm_pipe_mod_write, xen_shm_pipe_conv_writer_offers)) {
        perror("Pipe init");
        return -1;
    }



    if(argc>1 && sscanf(argv[1], "%"SCNu32, &dist_domid)) {
        printf("Using distant domain id: %"PRIu32"\n", dist_domid);
    } else {

        printf("Distant domain id: ");
        if((scanf("%"SCNu32, &dist_domid)!=1)) {
            printf("Scanf error");
            return -1;
        }
    }


    if(xen_shm_pipe_offers(xpipe, PAGE_COUNT, dist_domid, &local_domid, &grant_ref)) {
        perror("Pipe get domid");
        return -1;
    }

    printf("Local domain id: %"PRIu32"\n", local_domid);
    printf("Grant reference id: %"PRIu32"\n", grant_ref);

    printf("Will now wait for at most 30 seconds\n");
    if(xen_shm_pipe_wait(xpipe, 30*1000)) {
        perror("Pipe wait");
        return -1;
    }

    printf("Connected successfully !\n");
    printf("Start transmitting stdin\n");

    signal(SIGINT, clean);

    checksum = 0;
    sent_bytes = 0;


    while((stdin_read = read(0,buffer, BUFFER_SIZE))>0 ) {
        msg_len = (size_t) stdin_read;
        offset = 0;
        while(offset != msg_len) {
            retval = xen_shm_pipe_write(xpipe, buffer+offset, msg_len-offset);
            if(retval <= 0) {
                perror("xen pipe write");
                clean(0);
                return 0;
            }
            sent_bytes+=(uint64_t) retval;
            for(i=0; i<retval; ++i) {
                checksum = checksum + ((uint32_t) buffer[ ((int) offset) + i] + 10)*((uint32_t) buffer[ ((int) offset) + i] + 20);
                //printf("Checksum with '%"PRIu8"' -- %"PRIu32"\n", buffer[ ((int) offset) + i], checksum);
            }
            offset+= (size_t) retval;
            printf("\r%"PRIu64, sent_bytes);
            fflush(stdout);
        }


    }

    if(stdin_read==0) {
        printf("Stdin closed\n");
    } else {
        perror("stdin read");
    }

    clean(0);
    return 0;

}


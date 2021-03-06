#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <malloc.h>
#include <sys/time.h>

#include "../xen_shm_pipe.h"

//#define SHOW_STATS

static uint8_t page_count;
static uint32_t buffer_size;
static uint32_t iterations;
static uint64_t byte_count;
static xen_shm_pipe_p xpipe;
static struct timeval start;
static struct timeval stop;
static int pipe_used;


void usage(void);
void pipe_read(void);
void pipe_write(void);
void init_pipe_reader(void);
void init_pipe_writer(void);
void read_pc_and_size(int argc, char **argv);
void pipe_reader(int argc, char **argv);
void pipe_writer(int argc, char **argv);
void pipe_ramwriter(int argc, char **argv);


static void
clean(int sig)
{
    uint64_t usec_interval;
    double seconds;
    double bandwidth;


#ifdef XSHMP_STATS
    struct xen_shm_pipe_stats stats;
#endif
    gettimeofday(&stop , NULL);
    printf("\n");
    if(sig >= 0) {
        printf("Signal received: %i\n", sig);
    }

    printf("Byte count : %"PRIu64"\n", byte_count);
    if(byte_count) {
        usec_interval = (uint64_t) ((stop.tv_sec*1000000 + stop.tv_usec) - (start.tv_sec*1000000 + start.tv_usec));
        seconds = ((double) usec_interval)/1000000.0;
        bandwidth = ((double) byte_count)/((double) usec_interval); //MBps
        printf("Time lapse : %f seconds\n", seconds);
        printf("Bandwidth  : %f MBps  =   %f Mbps\n", bandwidth, bandwidth*8);
    }

#ifdef XSHMP_STATS
    stats = xen_shm_pipe_get_stats(xpipe);
    printf("\nWait calls   : %"PRIu64"\n", stats.ioctl_count_await);
    printf("Signal calls : %"PRIu64"\n", stats.ioctl_count_ssig);
    printf("Write calls  : %"PRIu64"\n", stats.write_count);
    printf("Read calls   : %"PRIu64"\n", stats.read_count);
    printf("Waiting      : %"PRIu8"\n", stats.waiting);
    printf("Epipe Prone  : %"PRIu64"\n", stats.ioctl_count_epipe_prone);
#endif



    printf("Now closing the pipe\n");
    xen_shm_pipe_free(xpipe);

    exit(0);
}

void
usage(void)
{
    printf("Usage: reader <page_count> <buffer_size>\n");
    printf("  OR   writer <page_count> <message_size> <iterations>\n");
    printf("  OR   ram_writer <message_size> <iterations>\n");
    exit(-1);
}



void pipe_read(void) {
    uint8_t* buffer;
    ssize_t retval;

#if defined(XSHMP_STATS) && defined(SHOW_STATS)
    struct xen_shm_pipe_stats stats;
#endif

    if((buffer = malloc(sizeof(uint8_t)*buffer_size))== NULL) {
        printf("Memory error\n");
        clean(0);
    }
    gettimeofday(&start , NULL);
    while((retval = xen_shm_pipe_read_all(xpipe, buffer, buffer_size)) > 0) {
        byte_count += (uint64_t) retval;

#if defined(XSHMP_STATS) && defined(SHOW_STATS)
        stats = xen_shm_pipe_get_stats(xpipe);
        printf("\rWait: %"PRIu64" Signals: %"PRIu64" Write: %"PRIu64" Read: %"PRIu64" Waiting: %"PRIu8" ",
                stats.ioctl_count_await, stats.ioctl_count_ssig, stats.write_count, stats.read_count, stats.waiting);

#endif

    }

    if(retval == 0) {
        printf("End of file \n");
    } else {
        perror("Xen pipe read");
    }

    clean(0);

}

void pipe_write(void) {
    uint8_t* buffer;
    ssize_t retval;
    uint32_t i;
#if defined(XSHMP_STATS) && defined(SHOW_STATS)
    struct xen_shm_pipe_stats stats;
#endif

    if((buffer = malloc(sizeof(uint8_t)*buffer_size))== NULL) {
        printf("Memory error\n");
        clean(0);
    }

    for(i = 0; i<buffer_size; i++) {
        buffer[i] = 'u';
    }
    gettimeofday(&start , NULL);
    for(i=0; i<iterations; i++) {
        retval = xen_shm_pipe_write_all(xpipe, buffer, buffer_size);
        if(retval < 0) {
            perror("Xen pipe write");
            clean(0);
        } else if(retval == 0) {
            printf("Write retval = 0 !\n");
            clean(0);
        }
        byte_count+=(uint64_t) retval;

#if defined(XSHMP_STATS) && defined(SHOW_STATS)
    stats = xen_shm_pipe_get_stats(xpipe);
    printf("\rWait: %"PRIu64" Signals: %"PRIu64" Write: %"PRIu64" Read: %"PRIu64" Waiting: %"PRIu8" ",
            stats.ioctl_count_await, stats.ioctl_count_ssig, stats.write_count, stats.read_count, stats.waiting);

#endif

    }

    clean(0);

}


void init_pipe_reader(void) {
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;

    printf("\nInit: Reader - Receiver\n");
    if(xen_shm_pipe_init(&xpipe, xen_shm_pipe_mod_read, xen_shm_pipe_conv_writer_offers)) {
        perror("Pipe init");
        exit(-1);
    }

    pipe_used = 1;

    if(xen_shm_pipe_getdomid(xpipe, &local_domid)) {
        perror("Pipe get domid");
        clean(0);
    }

    printf("RECEIVER domain id: %"PRIu32"\n", local_domid);
    printf("Distant domain id: ");
    if((scanf("%"SCNu32, &dist_domid)!=1)) {
        printf("Scanf error");
        clean(0);
    }

    printf("Offerer's grant ref: ");
    if((scanf("%"SCNu32, &grant_ref)!=1)) {
        printf("Scanf error");
        clean(0);
    }



    if(xen_shm_pipe_connect(xpipe,page_count,dist_domid, grant_ref)) {
        perror("Pipe connect");
        clean(0);
    }

    printf("Connected successfully !\n");
    signal(SIGINT, clean);

}


void init_pipe_writer(void) {
    uint32_t local_domid;
    uint32_t dist_domid;
    uint32_t grant_ref;


    printf("Init: Writer - Offerer\n");

    if(xen_shm_pipe_init(&xpipe, xen_shm_pipe_mod_write, xen_shm_pipe_conv_writer_offers)) {
        perror("Pipe init");
        exit(-1);
    }

    pipe_used = 1;

    printf("Distant domain id: ");
    if((scanf("%"SCNu32, &dist_domid)!=1)) {
        printf("Scanf error");
        clean(0);
    }


    if(xen_shm_pipe_offers(xpipe, page_count, dist_domid, &local_domid, &grant_ref)) {
        perror("Pipe get domid");
        clean(0);
    }

    printf("Local domain id: %"PRIu32"\n", local_domid);
    printf("Grant reference id: %"PRIu32"\n", grant_ref);

    printf("Will now wait for at most 30 seconds\n");
    if(xen_shm_pipe_wait(xpipe, 30*1000)) {
        perror("Pipe wait");
        clean(0);
    }

    printf("Connected successfully !\n");

    signal(SIGINT, clean);

}

void read_pc_and_size(int argc, char **argv) {
    byte_count = 0;

    if(sscanf(argv[2], "%"SCNu8, &page_count) ) {
        printf("Page count: %"PRIu8"\n", page_count);
    } else {
        printf("Invalid page count\n");
        usage();
    }

    if(sscanf(argv[3], "%"SCNu32, &buffer_size) ) {
        printf("Size: %"PRIu32"\n", buffer_size);
    } else {
        printf("Invalid size\n");
        usage();
    }
}

void pipe_reader(int argc, char **argv) {

    if(argc < 4) {
        usage();
    }

    read_pc_and_size(argc, argv);

    init_pipe_reader();

    pipe_read();
}



void pipe_writer(int argc, char **argv) {

    if(argc < 5) {
        usage();
    }

    read_pc_and_size(argc, argv);

    if(sscanf(argv[4], "%"SCNu32, &iterations) ) {
        printf("Iterations: %"PRIu32"\n", iterations);
    } else {
        printf("Invalid size\n");
        usage();
    }

    init_pipe_writer();

    pipe_write();
}

void pipe_ramwriter(int argc, char **argv) {
    uint8_t* buffer;
    uint8_t* buffer_2;
    uint32_t i;
    uint32_t j;
    uint32_t aligned_iter;

    if(argc < 4) {
        usage();
    }

    if(sscanf(argv[2], "%"SCNu32, &buffer_size) ) {
        printf("Size: %"PRIu32"\n", buffer_size);
    } else {
        printf("Invalid size\n");
        usage();
    }

    if(sscanf(argv[3], "%"SCNu32, &iterations) ) {
        printf("Iterations: %"PRIu32"\n", iterations);
    } else {
        printf("Invalid size\n");
        usage();
    }

    if((buffer = malloc(sizeof(uint8_t)*buffer_size))== NULL) {
        printf("Memory error\n");
        clean(0);
    }

    if((buffer_2 = malloc(sizeof(uint8_t)*buffer_size))== NULL) {
        printf("Memory error\n");
        clean(0);
    }

    for(i = 0; i<buffer_size; i++) {
        buffer[i] = 'u';
    }

    aligned_iter = buffer_size/8;

    byte_count = 0;
    gettimeofday(&start , NULL);
    for(i=0; i<iterations; i++) {
        for(j=0; j<aligned_iter ; j++) {
            *((uint64_t*) (&buffer[8*j])) = *((uint64_t*) (&buffer_2[8*j]));
        }
        j = j*8;
        for(; j<buffer_size; j++) {
            buffer_2[j] = buffer[j];
        }
        byte_count+=buffer_size;
    }

    clean(0);


}


int main(int argc, char **argv) {
    if(argc < 2) {
        usage();
    }

    pipe_used = 0;

    if(strcmp(argv[1], "reader") == 0) {
        pipe_reader(argc, argv);
    } else if(strcmp(argv[1], "writer")==0) {
        pipe_writer(argc, argv);
    } else if(strcmp(argv[1], "ram_writer")==0) {
        pipe_ramwriter(argc, argv);
    }
    usage();
    return -1;
}


#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/**
 * Implement the UNIX program sleep for xv6; your sleep should pause for a user-specified number of ticks. 
 * A tick is a notion of time defined by the xv6 kernel, namely the time between two interrupts from the timer chip.
 */
int
main(int argc, char *argv[])
{
    if(argc != 2){
        fprintf(2, "sleep: incorrect number of arguments\nusage: sleep n\n");
        exit(1);
    }
    int n = atoi(argv[1]);
    sleep(n);
    exit(0);
}
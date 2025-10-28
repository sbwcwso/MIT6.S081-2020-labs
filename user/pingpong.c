/**
 * Write a program that uses UNIX system calls to ''ping-pong'' a byte between two processes over a pair of pipes, one for each direction. The parent should send a byte to the child; the child should print "<pid>: received ping", where <pid> is its process ID, write the byte on the pipe to the parent, and exit; the parent should read the byte from the child, print "<pid>: received pong", and exit. Your solution should be in the file user/pingpong.c.
 */

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h" 

int 
main(int argc, char *argv[]) 
{
    int p[2];
    pipe(p);
    char buf[1];

    if (fork() == 0) {
        read(p[0], &buf, 1);
        printf("%d: received ping\n", getpid());
        write(p[1], buf, 1); 
        close(p[0]);
        close(p[1]);
        exit(0);
    } else { 
        write(p[1], "x", 1); 
        wait(0);
        read(p[0], &buf, 1); 
        printf("%d: received pong\n", getpid());

        close(p[0]);
        close(p[1]);
        exit(0);
    }
}
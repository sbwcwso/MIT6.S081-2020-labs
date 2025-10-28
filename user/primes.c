/**
 * Write a concurrent version of prime sieve using pipes. This idea is due to Doug McIlroy, inventor of Unix pipes. The picture halfway down https://swtch.com/~rsc/thread/ and the surrounding text explain how to do it. 
 */


/*
p = get a number from left neighbor
print p
loop:
    n = get a number from left neighbor
    if (p does not divide n)
        send n to right neighbor
*/
 
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h" 

#define MAX_NUMBER 35

static void
worker()
{
  int prime;
  int number;
  int p[2];

  if(read(0, &prime, sizeof(int)) <= 0) exit(1);  // No data input, error state
  printf("prime %d\n", prime);  // The first number in a new worker is a prime

  if(read(0, &number, sizeof(int)) > 0){  // The input number greater than one, recruit new worker
    pipe(p);
    if(fork() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      worker();
    }
    // Current worker finish the work and send data to the next
    close(p[0]);
    do
      if(number % prime != 0)
        write(p[1], &number, sizeof(int));
    while(read(0, &number, sizeof(int)) > 0);

    close(p[1]);
    wait((int *) 0);
  }
  exit(0);
}


int
main(void)
{
  int p[2];
  pipe(p);

  if(fork() == 0){
    close(0);
    dup(p[0]);  // redirect the input
    close(p[0]);
    close(p[1]);
    worker();  // recruit worker
  }

  close(p[0]);
  for(int i = 2; i <= MAX_NUMBER; i+=1)  // Send data
    write(p[1], &i, sizeof(int));
  close(p[1]);
  wait(0);

  exit(0);
}

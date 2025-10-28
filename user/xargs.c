/*
** Write a simple version of the UNIX xargs program: read lines from the standard input and run a command for each line, supplying the line as arguments to the command. 
*/

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

#define LINE_SIZE 512

char *
read_argument(void)
{
  static char argument[LINE_SIZE + 1];
  int i;
  for(i = 0; i < LINE_SIZE; i++)
    if(read(0, argument + i, 1) != 1 ||  //EOF
       argument[i] == '\n')              //new_line
      break;
  if(i == LINE_SIZE){
    fprintf(2, "Too long line\n");
    exit(1);
  }
  if(i == 0)
    return 0;  //EOF
  argument[i] =  0;  //replace '\n' with 0
  return argument;
}

int
main(int argc, char **argv)
{
  char *argument;
  char *new_argv[MAXARG];
  char *command;
  int i;

  if(argc < 2){
    fprintf(2, "Usage: xargs command [argument, ...]");
    exit(1);
  }

  command = *++argv;
  i = 0;
  while((new_argv[i++] = *argv++) != 0)
    ;

  while((argument = read_argument()) != 0){
    if(fork() == 0){
      new_argv[i - 1] = argument;
      new_argv[i] = 0;
      exec(command, new_argv);
      fprintf(2, "Command %s not found!\n", command);
      exit(1);
    }
    wait((int *)0);
  }
  exit(0);
}

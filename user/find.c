/**
 * Write a simple version of the UNIX find program: find all the files in a directory tree with a specific name.
 */

 #include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

#define STRCMP(a, R, b) (strcmp(a, b) R 0)

char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));
  return buf;
}

int
find(char *path, char *filename)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
    fprintf(2, "find: path %s too long\n", path);
    return -1;
  }

  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return -1;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return -1 ;
  }

  if(st.type != T_DIR){
    fprintf(2, "find: %s is not a directory\n", path);
    close(fd);
    return -1;
  }

  strcpy(buf, path);
  p = buf + strlen(buf);
  *p++ = '/';
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;
    if(stat(buf, &st) < 0){
      printf("ls: cannot stat %s\n", buf);
      continue;
    }
    
    switch(st.type){
    case T_FILE:
      if(STRCMP(de.name, == , filename))
        printf("%s\n", buf);
      break;

    case T_DIR:
      if(STRCMP(de.name, !=, ".") && STRCMP(de.name, !=, ".."))
        find(buf, filename);
      break;
    }
  }
  close(fd);
  return 0;
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    printf("Usage: find dirname filename.\n");
    exit(1);
  }
  
  if(find(argv[1], argv[2]) == -1)
    exit(1);

  exit(0);
}

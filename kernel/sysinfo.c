#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_sysinfo(void)
{
  uint64 sysinfo_addr;
  struct sysinfo info;

  if(argaddr(0, &sysinfo_addr) < 0)
    return -1;

  info.freemem = getfreemem();
  info.nproc = getnproc();

  if(copyout(myproc()->pagetable, sysinfo_addr, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;
  // int n, j;

  // struct proc *p = myproc();

  
  // pte_t *pte, *kpte;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;

  // my code
  
  // if(n > 0) {
  //   for (j = addr; j < addr + n; j += PGSIZE) {
  //     pte = walk(p->pagetable, j, 0);
  //     kpte = walk(p->kpagetable, j ,1);
  //     *kpte = (*pte) & ~PTE_U;
  //   }
  // } else {
  //   for (j = addr; j < addr + n; j -= PGSIZE) {
  //     uvmunmap(p->kpagetable, j, 1, 0);
  //   }
  // }

  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


uint64
sys_trace(void)
{
  int n;
  argint(0, &n);
  myproc()->tracemask = n;
  return 0;
}


uint64 
sys_sysinfo(void)
{
  uint64 addr;
  argaddr(0, &addr);

  struct proc *p = myproc();
  struct sysinfo info;

  info.freemem = get_free_mem();
  info.nproc = get_proc_num();

  if (copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0) // 把内核空间的info拷贝到对应程序的虚拟地址空间上
    return -1;
  return 0;
}


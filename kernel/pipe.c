#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  // 获取管道锁，避免管道中数据错乱
  acquire(&pi->lock);
  // 遍历每一个要写入buffer的字节
  while(i < n){
    // 如果进程已经关闭或者管道读端已经关闭，释放管道锁，写入失败
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      // 唤醒在nread上睡眠的进程，让它们从管道中取数据
      wakeup(&pi->nread);
      // 在nwrite上睡眠
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      // 实际复制数据到data缓冲区
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1)
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  // 写入完成，唤醒在nread上睡眠的进程，释放管道锁
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  // 获取管道锁以避免读写混乱
  acquire(&pi->lock);
  // 在管道为空（并且写入端是开启的）这个条件上循环
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    // 如果进程已经被杀死，释放管道锁，读取宣告失败
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
     // 如果管道为空且写入端开启（可能有人会写入数据），在nread上睡眠
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  // 实际读取
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1)
      break;
  }
  // 因为已经读了数据，所以管道中可能已经有一些空间了，唤醒在nwrite上睡眠的写进程
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  // 释放管道锁
  release(&pi->lock);
  return i;
}

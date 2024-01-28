#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//https://jianzzz.github.io/2017/08/20/%E7%AC%94%E8%AE%B009-xv6-%E9%83%A8%E5%88%86%E7%B3%BB%E7%BB%9F%E8%B0%83%E7%94%A8%E7%9A%84%E5%AE%9E%E7%8E%B0/
//exec的整体思路是：
//1、根据程序路径查找对应的inode；
//2、根据inode读取程序的elf信息；
//3、调用setupkvm()创建页目录，并映射内核空间（页目录项所存页表的权限是用户可读写，二级页表项所存物理页的权限按照kmap设定）；
//4、根据inode和elf程序头表偏移量，按段读取程序。每次循环时：
//4.1、先读取段的程序头表项，程序头表项包含段的虚拟地址及段占据的内存大小，
//4.2、根据信息调用allocuvm()分配出物理空间，分配过程会将程序的虚拟地址映射到分配到的物理页地址上，二级页表项所存物理页的权限是用户可读写，
//4.3、根据程序头表项信息（段偏移及段的文件大小）读取段到内存中；
//5、继续为程序分配两页物理空间，二级页表项所存物理页的权限是用户可读写，取消第一页用户可读写权限，第二页将作为用户栈。
//6、构建用户栈数据（参数等，见下）；
//7、更新进程的页目录、用户空间size、进程的tf->eip为elf.entry(main)、tf->esp为用户栈指针当前位置，然后调用switchuvm()函数设置cpu环境，加载进程的页目录地址到cr3，释放旧页目录空间；
//8、返回trapret处，切换为用户态，返回执行新程序代码，不返回执行旧程序代码。
int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  // int i, off, j;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  // pte_t *pte, *kpte;

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  //读取程序的elf
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC)
    goto bad;
  //创建内核页目录，映射内核空间。实际上是创建二级页表，并在二级页表项上存储物理地址。
  //页目录项所存页表的权限是用户可读写,二级页表项所存物理页的权限按照kmap设定
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  //根据inode和elf程序头表偏移量, 按段读取程序
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    //读取段的程序头表项
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;

    // my code
    if(sz1 >= PLIC) {
      goto bad;
    }

    sz = sz1;
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // mycode
  // uvmunmap(p->kpagetable, 0, PGROUNDUP(oldsz)/PGSIZE, 0);
  // for (j = 0; j < sz; j += PGSIZE) {
  //   pte = walk(pagetable, j, 0);
  //   kpte = walk(p->kpagetable, j ,1);
  //   *kpte = (*pte) & ~PTE_U;
  // }

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  // 打印init的页表 （lab pagetable）
  if(p->pid==1) vmprint(p->pagetable, 0);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}

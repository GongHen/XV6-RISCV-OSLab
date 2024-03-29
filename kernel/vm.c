#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code. kernel.ld 会设置这个在内核代码结束那里.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
// 为内核创建一个直接映射页表
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  // uart 寄存器
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  // virtio mmio 磁盘接口
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  // 映射内核可执行代码和只读的数据
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  // 映射内核数据和我们将使用的物理内存
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  // 将陷阱的进出用到的蹦床（trampoline）映射到内核中最高的虚拟地址。
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  // 映射内核栈
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
// 初始化一个内核页表
void
kvminit(void)
{
  //  kernel_pagetable = kvmmake();
   kernel_pagetable = vmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
// 将h/w页表寄存器切换到内核的页表,
// 并启用分页
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
// 返回页表pagetable中PTE的地址
// 返回在页表pagetable中，虚拟地址va对应的PTE地址。如果`alloc != 0`，则自动创建必要的页表页
//
// 在risc-v Sv39模式中，有三级页表页。
// 一个页表页包含512个64位的PTE。
// 一个64位的虚拟地址被分割成了五个域：
//   39..63 -- 必须为0
//   30..38 -- 9位二级索引
//   21..29 -- 9位一级索引
//   12..20 -- 9位零级索引
//    0..11 -- 12位的页中字节偏移量

// walk会返回虚拟地址对应的最后一级页表项，若alloc!=0，并且va没有对应的页表项，则会为它创建出一个页表项。
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  // 从2级开始
  for(int level = 2; level > 0; level--) {
    // 获取每个级别的页表索引
    pte_t *pte = &pagetable[PX(level, va)];
    // 如果存在pte并且有效，合法，从PTE中取出下一级页表物理地址并替换pagetable
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      // 走到这里，说明不存在pte，或者该pte已经是无效pte
      // 如果不允许分配，或者尝试为该pte分配新的下一级页表失败，直接返回0
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      // 将刚刚创建的新页表填充0
      memset(pagetable, 0, PGSIZE);
      // 设置正确的pte
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
   // 返回最后一级页表中的PTE
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
// 查找虚拟地址，返回物理地址，
// 如果未映射，则为0。
// 只能用于查找用户页。
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
// 向内核页表中添加一个映射 
// 只在启动时被使用
// 该函数不会刷新TLB或（通知硬件）打开分页
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
// 为在va处开始的虚拟地址创建指向pa处的物理地址的PTEs
// va以及size不必须是页对齐的。
// 返回0成功，如果`walk()`函数无法分配一个需要的页表页则返回-1
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  // PGROUNDDOWN的含义就是取下面的第一个页对齐位置。（假如给定n=8244，则PGROUNDDOWN(n)=8192）。
  // [a, last]，就是要为用户分配的一段连续虚拟地址空间范围，首部和尾部都进行了向下页对齐 
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    // 如果索引pte失败，可能是内存不足了，直接返回-1
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    // 如果pte存在并且pte合法，说明我们要映射的虚拟地址上面已经有另一个映射了
    // 直接panic
    if(*pte & PTE_V)
      panic("mappages: remap");
    // 将物理地址转换成PTE形式（去掉offset部分，预留flag部分）
    // 并加上权限，以及有效flag
    *pte = PA2PTE(pa) | perm | PTE_V;
    // 如果a和last相等，说明已经无需再分配了。
    if(a == last)
      break;
    // a和pa一起递增，等待下一次循环
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}
 // my code
 int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// my code
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
}


// my code
void 
vmprint(pagetable_t pagetable, uint depth)
{
  if(depth == 0)
    printf("page table %p\n", pagetable);
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){
      for(int j = 0; j < depth; j++)
        printf(".. ");
      uint64 child = PTE2PA(pte);
      printf("..%d: pte %p pa %p\n", i, pte, child);
      if(depth < 2)
        // 如果层数等于 2 就不需要继续递归了，因为这是叶子节点
        vmprint((pagetable_t) child, depth + 1);
    }
  } 
}

// my code
/* 
 * 原来的kvmmap直接修改了kernel_pagetable，这里写一个新的kvmmap允许指定要映射VA-PA关系的页表
 */
void 
kvmmap2(pagetable_t pt, uint64 va, uint64 pa, uint64 sz, int perm)
{
    if(mappages(pt, va, sz, pa, perm) != 0)
        panic("kvmmap2");
}

pagetable_t
vmmake(void)
{
  pagetable_t pt = (pagetable_t) kalloc();
  memset(pt, 0, PGSIZE);

  // uart registers
  kvmmap2(pt, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap2(pt, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap2(pt, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap2(pt, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap2(pt, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap2(pt, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap2(pt, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  return pt;
}

// uint64
// kvmpa(uint64 va)
// {
//   uint64 off = va % PGSIZE;
//   pte_t *pte;
//   uint64 pa;

//   pte = walk(myproc()->kpagetable, va, 0); 
//   if(pte == 0)
//     panic("kvmpa");
//   if((*pte & PTE_V) == 0)
//     panic("kvmpa");
//   pa = PTE2PA(*pte);
//   return pa+off;
// }

// 仿照uvmunmap写一个kvmunmap,唯一的区别是不用回收物理内存
void 
kvmunmap(pagetable_t pagetable, uint64 va, uint64 size)
{
    uint64 a;
    pte_t *pte;

    if((va % PGSIZE) != 0)
        panic("kvmunmap: not aligned");

    for(a = va; a < va + size; a += PGSIZE){
        if((pte = walk(pagetable, a, 0)) == 0)
            panic("kvmunmap: walk");
        if((*pte & PTE_V) == 0)
            panic("kvmunmap: not mapped");
        if(PTE_FLAGS(*pte) == PTE_V)
            panic("kvmunmap: not a leaf");

        // 不需要销毁物理内存
        //uint64 pa = PTE2PA(*pte);
        //kfree((void*)pa);
        
        *pte = 0;
    }
}


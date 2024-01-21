// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// run是一个链表结构
struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  // freelist是一个空闲页的链表
  struct run *freelist;
} kmem;

void
kinit()
{
  // 对内存操作加锁，这把锁能够串行化内存分配
  initlock(&kmem.lock, "kmem");
  // https://www.cnblogs.com/lilpig/p/17180784.html
  // 将end到PHYSTOP的内存按页面加到freelist中
  // kinit以end作为起始，PHYSTOP作为结束调用freerange，
  // freerange的作用是把一个范围以页的形式挂载到freelist中
  freerange(end, (void*)PHYSTOP);
  // end从哪来？PHYSTOP是什么？指针类型为何是void，地址end未必是4096的倍数
  // xv6的内核最大内存是写死的128MB，在risc-v主板上，0x80000000是物理内存的起始地址，
  // xv6会将内核的虚拟地址空间中的0x80000000到0x86400000共128MB映射到了物理地址空间的相同位置。
  // KERNBASE就是内核物理内存的起始地址0x80000000，PHYSTOP就是内核物理内存的结束地址0x86400000。
  printf("end=%p PHYSTOP=%p\n",end, PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  // PGROUNDUP这个宏就是在C层面进行4K对齐
  // pa_start是未对齐的数字，而p则是比它稍大的一个已经对齐的数字。
  // 对齐会造成一点点的空间浪费，但是能极大的提高读写效率。
  // for循环，从可用的第一个页p开始，一直到pa_end，每次加一个页面大小，
  // 然后调用kfree去释放页面，实际上就是加到freelist中。
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  // 安全校验，地址是否按页面大小对齐，地址是否小于开始位置，大于等于最大内存位置
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  // 填充垃圾数据
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 加锁，向freelist链表头添加数据，更换freelist指向新的头，释放锁
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  // freelist变成了一个链表，每个链表项目是一个可以被分配使用的页面
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// 分配物理内存中的4098Bytes大小的页面
// 返回一个内核可用的指针
// 若无法分配内存，返回0
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

uint64 
get_free_mem(void)
{
  struct run *r;
  uint64 pages = 0; // 记录空闲页数

  acquire(&kmem.lock);
  r = kmem.freelist;
  while (r)
  {
    pages++;
    r = r->next;
  }
  release(&kmem.lock);

  return (pages << 12); // Book P29
  // better one: return (pages << PGSHIFT);
}



#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// https://github.com/ejunjsh/myxv6/blob/main/kernel/main.c
// start() jumps here in supervisor mode on all CPUs.
// 管理员模式
void
main()
{
  // 确保只有一个CPU执行一次
  if(cpuid() == 0){
    // Console驱动（console.c）是一个很简单的驱动结构，
    // console驱动通过RISC-V上的UART串口硬件接收人类输入的字符，驱动一次性积累一行输入，处理特殊输入字符（如backspace和control-u），
    // 像shell这样的用户进程，使用read系统调用去从console中获取输入行。当你向在QEMU中的xv6输入时，你的击键通过QEMU模拟的UART硬件传送到xv6中。
    // 驱动对面的UART硬件是一个QEMU模拟的16550芯片，在一个真实的计算机上，
    // 一个16550可能管理连接到一个终端或其它计算机的RS232串行链路。当我们运行QEMU时，它连接到你的键盘和显示器。
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    // kinit函数主要完成的功能是将物理内存页挂载到由内核维护的空闲链表中
    // 操作系统会暴露给应用进程用于分配内存的接口，比如alloc啥的，
    // 还会暴露用于归还内存的接口，比如free。
    // 在系统层面，内存的分配是以页为最小粒度的，
    // 操作系统会记录哪些物理页可以用于分配，
    // 在xv6中，完成这个功能的结构是kernel/kalloc.c文件中kmem结构体中的freelist链表
    // https://www.cnblogs.com/lilpig/p/17180784.html
    kinit();         // physical page allocator，物理页分配器
    // kinit 一切操作都是直接在物理内存做的，并没有做虚拟地址转换。
    // 在kvminit函数中挂载了内核页表，并做了一些基本的映射。
    // 调用kalloc分配一个页，作为内核页表，并将硬件设备、kernel text、kernel data做一个虚拟地址与实际物理地址相等的直接映射。
    kvminit();       // create kernel page table，创建内核页表
    // 切换硬件的页表寄存器到内核页表，并开启虚拟地址转换
    kvminithart();   // turn on paging，启用分页
    //至此，xv6启动时的内核页表挂载过程执行完成。

    procinit();      // process table，进程表

    // https://www.cnblogs.com/lilpig/p/17244493.html
    // 中断是一种trap），当一个设备发起了一个中断，内核的trap处理代码识别它并调用设备的中断处理器。
    // 在xv6中，这个调度发生在devintr
    // 很多设备驱动都在两个上下文中执行代码：上半部分运行在进程的内核线程中，下半部分在中断时执行。
    // 上半部分通过如read和write的系统调用被使用，表明我们想要设备执行I/O。这个代码可能请求硬件取执行一个操作（比如请求磁盘去读或写），
    // 然后，代码等待操作完成，最终，设备完成了操作，发起一个中断，此时，下半部分，也就是设备的中断处理器，
    // 认出是什么操作完成了，如果合适的话，唤醒一个等待的进程，并告诉硬件开始任何在等待中的后续操作。
    trapinit();      // trap vectors，陷阱向量
    trapinithart();  // install kernel trap vector，安装内核陷阱向量
    plicinit();      // set up interrupt controller，设置中断控制器
    plicinithart();  // ask PLIC for device interrupts，向PLIC配置设备中断
    binit();         // buffer cache，缓冲区缓存
    iinit();         // inode table，inode缓存
    fileinit();      // file table，文件表
    virtio_disk_init(); // emulated hard disk，模拟硬盘
    userinit();      // first user process，第一个用户进程
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging，启用分页
    trapinithart();   // install kernel trap vector，安装内核陷阱向量
    plicinithart();   // ask PLIC for device interrupts，向PLIC配置设备中断
  }

  scheduler();        
}

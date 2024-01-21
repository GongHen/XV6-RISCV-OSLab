#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
// entry.S 需要给每个cpu设置一个栈
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// a scratch area per CPU for machine-mode timer interrupts.
// 为每个cpu在机器模式下的定时中断分配一个暂存区域
uint64 timer_scratch[NCPU][5];

// assembly code in kernelvec.S for machine-mode timer interrupt.
// 这个是机器模式下的定时中断处理函数，定义在kernelvec.S
extern void timervec();

// entry.S jumps here in machine mode on stack0.
// 从entry.S跳到这里，函数用的栈为stack0，处在机器模式下
void
start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  // 设置M(机器模式)之前的特权模式为管理员模式，这样当mret的时候，就可以返回管理员模式了
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  // 设置M的程序计数器为main，这样mret的时候就会跳到main了
  // 需要 gcc -mcmodel=medany
  w_mepc((uint64)main);

  // disable paging for now.
  // 现在先禁用分页
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // 代理所有中断和异常到管理员模式
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // ask for clock interrupts.
  // 初始化时钟中断

  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // 设置每个cpu的hartid到tp寄存器，方便后面cpuid()函数调用
  int id = r_mhartid();
  w_tp(id);

  // switch to supervisor mode and jump to main().
  // 转换到管理员模式，并跳到main()
  asm volatile("mret");
}

// arrange to receive timer interrupts.
// they will arrive in machine mode at
// at timervec in kernelvec.S,
// which turns them into software interrupts for
// devintr() in trap.c.
// 设置机器模式下cpu能收到定时中断
// 这样中断到来时会跳到kernelvec.S下的timervec
// timervec会把这个中断转换成软件中断
// 然后跳到trap.c的devintr()函数
void
timerinit()
{
  // each CPU has a separate source of timer interrupts.
  // 每个cpu都有它自己的定时中断
  int id = r_mhartid();

  // ask the CLINT for a timer interrupt.
  // 请求CLINT给一个定时中断
  int interval = 1000000; // cycles; about 1/10th second in qemu. 周期；在qemu里，大概1/10秒
  *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;

  // prepare information in scratch[] for timervec.
  // scratch[0..2] : space for timervec to save registers.
  // scratch[3] : address of CLINT MTIMECMP register.
  // scratch[4] : desired interval (in cycles) between timer interrupts.
  // 为timervec准备信息在scratch[]
  // scratch[0..2] : 用来给timervec保存寄存器
  // scratch[3] : CLINT MTIMECMP 寄存器地址
  // scratch[4] : 保存定时中断之间的间隔（周期数）
  uint64 *scratch = &timer_scratch[id][0];
  scratch[3] = CLINT_MTIMECMP(id);
  scratch[4] = interval;
  w_mscratch((uint64)scratch);

  // set the machine-mode trap handler.
  // 设置机器模式下的陷阱(trap)处理函数
  w_mtvec((uint64)timervec);

  // enable machine-mode interrupts.
  // 启用机器模式下的所有中断
  w_mstatus(r_mstatus() | MSTATUS_MIE);

  // enable machine-mode timer interrupts.
  // 启用机器模式下的定时中断
  w_mie(r_mie() | MIE_MTIE);
}

// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};
  /*****************************************************************************
  //物理内存分配器的结构，其中包含的数据结构就是一个有可分配物理内存页构成的空闲
  //链表。分配器使用结构体run, 还用了一个spin lock 来保护空闲链表。 kfree将内存
  //加入到的空闲链表就是分配器。
  *****************************************************************************/
struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.

  /*****************************************************************************
  //kinit1初始化内核末尾到物理内存4M的物理内存空间为未使用
  //两者的区别在于kinit1调用前使用的还是最初的页表（也就是是上面的内存布局），
  //所以只能初始化4M，同时由于后期再构建新页表时也要使用页表转换机制来找到实际
  //存放页表的物理内存空间，这就构成了自举问题，xv6通过在main函数最开始处释放内
  //核末尾到4Mb的空间来分配页表，由于在最开始时多核CPU还未启动，所以没有设置锁机制。
  *****************************************************************************/
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  //使用freerange 将内存加入空闲链表，freearnge 则是通过kfree实现该功能。
  freerange(vstart, vend);
}
  /*****************************************************************************
  //kinit2初始化剩余内核空间到PHYSTOP为未使用
  //kinit2在内核构建了新页表后，能够完全访问内核的虚拟地址空间，
  //所以在这里初始化所有物理内存，并开始了锁机制保护空闲内存链表
  *****************************************************************************/
void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);//free全部物理地址
  kmem.use_lock = 1;
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)

  /*****************************************************************************
  //kfree 函数 首先将释放内存的每一字节设为1.这使得访问已被释放内存的代码所读到
  //的不是原有数据，而是垃圾数据。 这样做的目的是让这种错误的代码尽早崩溃。
  //接下来kfree 把v转换成一个指向结构体struct run 的指针，在r-> next 中保留原有
  //空闲列表的表头，然后将当前空闲链表设置为r(说白了就是将r加到空闲列表上)。
  //xv6中有一个物理内存分配器的结构，其中包含的数据结构就是一个有可分配物理内存
  //页构成的空闲链表。分配器使用结构体run, 还用了一个spin lock 来保护空闲链表。 
  //kfree将内存加入到的空闲链表就是分配器。
  *****************************************************************************/
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  //首先将释放内存的每一字节设为1.这使得访问已被释放内存的代码所读到的不是
  //原有数据，而是垃圾数据。 这样做的目的是让这种错误的代码尽早崩溃。
  memset(v, 1, PGSIZE);//全部置1

  if(kmem.use_lock)
    acquire(&kmem.lock);

  //把v转换成一个指向结构体struct run 的指针，在r-> next 中保留原有空闲列表的表头，
  //然后将当前空闲链表设置为r(说白了就是将r加到空闲列表上)。
  r = (struct run*)v;
  
  //将内存加入空闲链表
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
/*****************************************************************************
//首先是调用kalloc 函数从空闲链表上摘下一个内存段（4096字节），这就是分配了物
//理内存。返回一个内核可用的指针，如果分配失败返回0
*****************************************************************************/

char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}


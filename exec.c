#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

  /*************************************************************************
  //exec前驱知识，这部分代码不容易啊
  //一个运行中进程的用户内存结构。堆在栈之上，所以它可以增长（通过 sbrk ）。
  //栈占用了单独 的一页内存，其中存有 exec 创建的初始数据。栈的最上方放着字
  //符串形式的命令行参数以及指向这些参数的指针数组，其 下方放的值使得一个程
  //序可以从 main 开始，仿佛刚刚调用了函数 main(argc, argv) 。为了防止栈使用
  //了它不应该使用的 页，栈的下方有一个保护页。保护页没有映射，因此当栈的增
  //长超出其所在页时就会产生异常，因为无法翻译这个错误的地址。
  **************************************************************************/
  
  /*************************************************************************
  //exec 是创建地址空间中用户部分的系统调用。它根据文件系统中保存的某个文件
  //来初始化用户部分。 exec 通过 namei 打开二进制文件。然后，它读取 ELF 头。
  //xv6 应用程序以通行的 ELF 格式来 描述，该格式在 elf.h 中定义。一个 ELF 
  //二进制文件包括了一个 ELF 头，即结构体 struct elfhdr ，然后是连续 几个程
  //序段的头，即结构体 struct proghdr 。每个 proghdr 都描述了需要载入到内存
  //中的程序段。xv6 中的程序只有一个程序段的头，但其他操作系统中可能有多个。
  //exec第一步是检查文件是否包含ELF二进制代码。exec 通过setupkvm 分配了一个
  //没有用户部分映射的页表。再通过allocuvm 为每个ELF段分配内存，然后通过
  //loaduvm 把段的内容载入内存中。allocuvm 会检查请求分配的虚拟地址是否在
  //kernbase之下。
  **************************************************************************/
int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  begin_op();
  //通过 namei 打开二进制文件
  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  // 它读取 ELF 头
  // xv6 应用程序以通行的 ELF 格式来 描述，该格式在 elf.h 中定义
  // 检查文件是否包含ELF二进制代码
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;
  // 通过setupkvm 分配了一个没有用户部分映射的页表
  if((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  // 把进程装载到内存里
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
	// 再通过allocuvm 为每个ELF段分配内存，
	// allocuvm 会检查请求分配的虚拟地址是否在kernbase之下
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
	// 然后通过loaduvm 把段的内容 载入内存
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  // 更新文件节点的引用计数等信息
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.

  /*****************************************************************************
  //在接下来的页范围内分配两个页，第一个不可用，第二个是用户栈。
  //不可用页的用途： 
  //(1)当程序尝试使用超过一个页的栈时就会出错
  //(2)另外也让exec能够处理那些过于庞大的参数；
  //   当参数过于庞大时，exec 用于将参数拷贝到栈上的函数copyout会发现
  //   目标页无法访问，并且返回-1.
  *****************************************************************************/
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;
  // 切换到用户进程,装载新映像
  switchuvm(curproc);
  // 释放老的页表 以及处于用户态的物理内存页。
  freevm(oldpgdir);
  return 0;

  /*****************************************************************************
  //    goto bad说明
  //    在创建新的内存映像时，如果 exec 发现了错误，比如一个无效的程序段，
  //它就会跳转到标记 bad 处，释放这段内存映像， 然后返回 -1
  //    exec 必须在确认该调用可以成功后才能释放原来的内存映像，
  //否则，若原来的内存映像被释放， exec 甚至都无法向它返回 -1 
  //    exec 中的错误只可能发生在新建内存映像时。
  //一旦新的内存映像建立完成， exec 就能装载新映像 （switchuvm(curproc);）
  //而把旧映像释放（freevm(oldpgdir);）。最后， exec 成功地返回 0
  *****************************************************************************/

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

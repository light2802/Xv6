#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

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
  curproc->page_inserted = 0;
  curproc->page_fault_count = 0;
  free_backstore(curproc);
  curproc->blist = 0;

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  safestrcpy((curproc->path), path, strlen(path) + 1);
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
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
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    //Dont load here. Load from disk when needed
    //Save info for from where to load later, only 2 sections code, data
  }
  iunlockput(ip);
  end_op();
  ip = 0;
  curproc->elf_size = sz; // size of code+data+bss 

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  char* buffer = curproc->buf;
  sp = PGSIZE;

  // Push argument strings in buffer, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    safestrcpy(&(curproc->buf[sp]), argv[argc], strlen(argv[argc]) + 1);
    ustack[3+argc]=PGROUNDUP(curproc->elf_size) + PGSIZE + sp;
    //if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
    //  goto bad;
    //ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = PGROUNDUP(curproc->elf_size) + PGSIZE + (sp - (argc+1)*4);  // argv pointer

  sp -= (3+argc+1) * 4;
  memmove(buffer+sp, ustack, (3+argc+1)*4);
  if(store_page(curproc, sz - PGSIZE) < 0)
      panic("no space to store stack in backstore");
  //if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
  //  goto bad;

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
  curproc->tf->esp = PGROUNDUP(curproc->elf_size) + PGSIZE + sp;
  curproc->alloc = 0;
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

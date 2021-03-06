#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "backstore.h"
#include "file.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm, int alloc)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | alloc;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W | PTE_P}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0 | PTE_P},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W | PTE_P}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W | PTE_P}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm | PTE_P, 0) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U|PTE_P, 0);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem = (char*)V2P(0);
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
      if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U, 0) < 0){
          cprintf("allocuvm out of memory (2)\n");
          return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

void
clearptep(pde_t *pgdir, char *uva)
{
  pte_t *pte;
  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearptep");
  *pte &= ~PTE_P;
}
// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(struct proc* dest, struct proc* src)
{
  pde_t *d;
  pte_t *pte;
  uint pa = 0, flags = 0, i;
  char *mem = 0;
  int alloc = 0;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < src->elf_size; i += PGSIZE){
    if((pte = walkpgdir(src->pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P)){
        if(mappages(d, (char*)i, PGSIZE, V2P(mem), PTE_W | PTE_U, 0) < 0)
            goto bad;
    }
    else{
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    alloc = PTE_ALLOC(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    }
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags, alloc) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  pte = walkpgdir(src->pgdir, (char *)(PGROUNDUP(src->elf_size) + PGSIZE), 0);
  if(*pte & PTE_P){
      memmove(dest->buf, (char *)(PGROUNDUP(src->elf_size) + PGSIZE), PGSIZE);
      if(store_page(dest, (PGROUNDUP(src->elf_size) + PGSIZE)) < 0)
          panic("no space to store stack in backstore\n");
  }
  else{
      load_frame(dest->buf, (char *)(PGROUNDUP(src->elf_size) + PGSIZE));
      if(store_page(dest, (PGROUNDUP(src->elf_size) + PGSIZE)) < 0)
          panic("no space to store stack in backstore\n");
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

void page_fault_handler(unsigned int fault_addr){
    uint alloc;
    if((uint)fault_addr >= KERNBASE){
	    cprintf("crossed the boundary of the user memory\n");
	    myproc()->killed = 1;
	    return;
    }
    fault_addr = PGROUNDDOWN(fault_addr);
    struct proc *currproc = myproc();
    currproc->page_fault_count++;
    struct elfhdr elf;
    struct proghdr ph;
    struct inode *ip;
    int i, off;
    char *mem;
    int loaded = 0;
    while((mem = kalloc()) == 0){
	    currproc->page_inserted++;
	    replace_page(currproc);
    }
    if(currproc->alloc < 8)
	    (currproc->alloc) += 1;
    alloc = GETALLOC(((currproc->alloc) - 1));
    if(mappages(currproc->pgdir, (char *)fault_addr, PGSIZE, V2P(mem), PTE_W | PTE_U | PTE_P, alloc) < 0)
	    panic("mappages");
    if((fault_addr > currproc->elf_size ||  currproc->code_on_bs) && load_frame(mem, (char *)fault_addr) == 1){
	    loaded = 1;
	    loaded++;
    }
    else{
	    begin_op();
	    ip = namei(currproc->path);
	    if(ip == 0)
	        panic("Namei path");
	    ilock(ip);
	    readi(ip, (char *)&elf, 0, sizeof(elf));
	    for(i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)){
	        if(readi(ip, (char *)&ph, off, sizeof(ph)) != sizeof(ph))
		        panic("Prog header unable to read");
	        if(ph.vaddr <= fault_addr && fault_addr <= ph.vaddr + ph.memsz){
		        if(ph.vaddr + ph.filesz >= fault_addr + PGSIZE)
		            loaduvm(currproc->pgdir, (char *)(ph.vaddr + fault_addr), ip, ph.off + fault_addr, PGSIZE);
		        else{
		            if(fault_addr >= ph.filesz){
			            if(fault_addr + PGSIZE <= ph.memsz)
			                stosb(mem, 0, PGSIZE);
			            else
			                stosb(mem, 0, ph.memsz - fault_addr);
		            }
		            else{
			            loaduvm(currproc->pgdir, (char *)(ph.vaddr + fault_addr), ip, ph.off + fault_addr, ph.filesz - fault_addr);
			            stosb((mem + (ph.filesz - fault_addr)), 0, PGSIZE - (ph.filesz - fault_addr)); 
			            ip = namei(currproc->path);
		            }
		        }
	        }
	    }
	    iunlockput(ip);
	    end_op();
    }
}
void replace_page(struct proc *currproc){
    pte_t *pte;
    uint i;
    uint alloc = 8, pa = 0, min_va = currproc->sz;
    uint flags;
    char reach_alloc_max = 0;
    uint pte_alloc;
    if(currproc->alloc == 0)
	    panic("global replacement");
    else{
	    for(i = 0; i < (currproc->sz); i += PGSIZE){
	        if(i == PGROUNDUP(currproc->elf_size))
		        continue;
	        if((pte = walkpgdir(currproc->pgdir, (void *)i, 0)) == 0)
		        panic("page replacement : copyuvm should exist");
	        if(*pte & (PTE_P)){
		        pa = PTE_ADDR(*pte);
		        flags = PTE_FLAGS(*pte);
		        if(alloc > (PTE_ALLOC(*pte) >> 9)){
		            alloc =  PTE_ALLOC(*pte) >> 9;
		            min_va = i;
		        }
		        if((uint)(PTE_ALLOC(*pte) >> 9) > 0 ){
		            pte_alloc = PTE_ALLOC(*pte) >> 9;
		            if(pte_alloc == 7 ){
			            if(reach_alloc_max == 0)
			                reach_alloc_max = 1;
		            }
		            *pte = pa |  GETALLOC((pte_alloc - 1)) | flags ;
		        }
            }
	    }
	    pte = walkpgdir(currproc->pgdir, (void *)min_va, 0);
	    pa = PTE_ADDR(*pte);
	    memmove((currproc->buf), (char *)P2V(pa), PGSIZE);
	    if(store_page(currproc, min_va) == -1)
		    panic("Backing store size over");
	    *pte = pa | PTE_W | PTE_U;
	    char *va = P2V(pa);
	    currproc->code_on_bs = 1;
	    kfree(va);
    }
}
int load_frame(char *pa, char *va){
    struct buf *buff;
    struct proc *currproc = myproc();
    int j;
    struct backstore_frame *temp = currproc->blist;
    int current_index;
    uint block_no;
    while(1){
	    if((char *)(temp->va) == va){
	        current_index = ((uint)temp - (uint)(backstore.backstore_bitmap)) / sizeof(struct backstore_frame);
	        block_no = BACKSTORE_START + current_index * 8;
	        break;
	    }
	    if(temp->next_index == -1)
	        return -1;
	    temp = &(backstore.backstore_bitmap[temp->next_index]);
    }
    for(j = 0; j < 8; j++){
	    buff = bread(ROOTDEV, (block_no) + j);
	    memmove((pa + BSIZE * j), buff->data, BSIZE);
	    brelse(buff);
    }
    return 1;
}
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.


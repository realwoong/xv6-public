// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
//pa4 추가
#include <stddef.h>

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

//pa4 skel
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;
//pa4 skel

//pa4 add
pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc);
int alloc_swap_block(void);
//pa4 add

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);

  //____pa4 add for 페이지 구조체 배열 초기화, LRU리스트초기화

  for (int i = 0; i < PHYSTOP / PGSIZE; i++) {
        pages[i].next = NULL;
        pages[i].prev = NULL;
        pages[i].pgdir = NULL;
        pages[i].vaddr = (char *)(i * PGSIZE);
    }

    // LRU 리스트 초기화
    page_lru_head = NULL;
    num_free_pages = 0;
    num_lru_pages = 0;
    //____pa4추가

}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;

  //____pa4
  // LRU 리스트 초기화
    for (char *p = (char *)PGROUNDUP((uint)vstart); p + PGSIZE <= (char *)vend; p += PGSIZE) {
        struct page *pg = &pages[V2P(p) / PGSIZE];
        if (!page_lru_head) {
            page_lru_head = pg;
            pg->next = pg;
            pg->prev = pg;
        } else {
            pg->next = page_lru_head;
            pg->prev = page_lru_head->prev;
            page_lru_head->prev->next = pg;
            page_lru_head->prev = pg;
        }
        num_free_pages++;
        num_lru_pages++;
    }
    //____pa4추가
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;
  //pa4
  struct page *pg;
  //pa4

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);

  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;

  //pa4 추가
  pg = &pages[V2P(v) / PGSIZE];
    if (page_lru_head) {
        pg->next = page_lru_head;
        pg->prev = page_lru_head->prev;
        page_lru_head->prev->next = pg;
        page_lru_head->prev = pg;
    } else {
        page_lru_head = pg;
        pg->next = pg;
        pg->prev = pg;
    }
    num_free_pages++;
    num_lru_pages++;
    //pa4


  if(kmem.use_lock)
    release(&kmem.lock);
}


//pa4에서 추가
int reclaim(void) {
    struct page *p = page_lru_head;

    while (p != NULL) {
        pte_t *pte = walkpgdir(p->pgdir, p->vaddr, 0);
        if (!pte || !(*pte & PTE_U)) {
            // 사용자 페이지가 아닌 경우 건너뜁니다.
            p = p->next;
            continue;
        }
        if (*pte & PTE_A) {
            // 접근 비트가 설정되어 있으면 클리어하고 다음 페이지로 이동
            *pte &= ~PTE_A;
            p = p->next;
        } else {
            // 접근 비트가 설정되지 않은 페이지를 스왑 아웃
            int swapblk = alloc_swap_block();
            if (swapblk == -1) {
                panic("reclaim: no swap space available");
            }
            swapwrite(p->vaddr, swapblk);
            // 페이지 테이블 엔트리 업데이트
            *pte = PTE_SWAP | (swapblk << PTE_SHIFT);
            if (p->prev)
                p->prev->next = p->next;
            if (p->next)
                p->next->prev = p->prev;
            if (p == page_lru_head)
                page_lru_head = p->next;
            num_lru_pages--;
            return 1;
        }
    }
    return 0;
}
//pa4에서 추가
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;
  int reclaimed = 0;

try_again:
  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;

  // 페이지 교체 시도
  if(!r && !reclaimed) {
    if(reclaim()) {
      reclaimed = 1;
      if(kmem.use_lock)
        release(&kmem.lock);
      goto try_again;
    } else {
      // 메모리 부족 시 에러 메시지 출력
      if (kmem.use_lock)
        release(&kmem.lock);
      cprintf("kalloc: out of memory\n");
      return 0;
    }
  }

  if(r)
    kmem.freelist = r->next;
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

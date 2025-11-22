#include <stdlib.h>

#define PAGE_SIZE 4096
#define TAG(ptr, tag) (((ptr) & 0xfffffffc) | ((tag) & 0x4))
#define TAGOF(ptr) ((ptr) & 0x4)
#define UNTAG(ptr) ((ptr) & 0xfffffffc)

/* Memory page item */
struct header {
        unsigned int size;
        struct header *next;
};

static int initialized = 0;

static void *stack_base;
static void *stack_top;

/* Circular linked lists */
static struct header base;
static struct header *freep = &base;
static struct header *usedp;

/*
  Given a pointer to an allocated block, locate the corresponding gap in the free list where
  that block had been allocated, and add the newly deallocated block to that list
*/
static void add_to_free(struct header *block) {
        struct header *cur;

        /* Iterate through to find the pointers `block` is between */
        for (cur = freep; !(block > cur && block < cur->next); cur = cur->next) {
                if (cur >= cur->next && (block > cur || block < cur->next)) {
                        break;
                }
        }

        /* Set the newly freed block to point at the next free block */
        if (block + block->size == cur->next) {
                block->size += cur->next->size;
                block->next = cur->next->next;
        } else {
                block->next = cur->next;
        }

        /* Set the previous block to point at the current block */
        if (cur + cur->size == block) {
                cur->size += block->size;
                cur->next = block->next;
        } else {
                cur->next = block;
        }

        freep = cur;
}

/*
  Request more memory from the kernel
*/
static struct header *morecore(size_t num_units) {
        void *p; /* Pointer to location where new blcok will be added */
        struct header *new_block; /* Pointer to new block */

        /* Force the allocation to be at least the page size */
        if (num_units < PAGE_SIZE) {
                num_units = PAGE_SIZE / sizeof(struct header);
        }

        /* Attempt to allocate new memory and ensure it doesn't fail */
        if ((p = sbrk(num_units * sizeof(struct header))) == (void*)-1) {
                return NULL;
        }

        /* Update pointer at end to reflect new memory */
        new_block = (struct header*)p;
        new_block->size = num_units;
        add_to_free_list(new_block);

        return freep;
}

/*
  Allocate a new blcok of size at least `alloc_size` and return a pointer
*/
void *dumpster_alloc(size_t alloc_size) {
        size_t units;
        struct header *cur, *prev;

        /* Align units of allocation to header size, plus one for an actual header */
        units = (alloc_size + sizeof(struct header) - 1) / sizeof(struct header) + 1;
        prev = freep;

        for (cur = prev->next;; prev = cur, cur = prev->next) {
                if (cur->size < num_units) {
                        if (cur == freep) {
                                cur = morecore(units);
                                if (cur == NULL) {
                                        return NULL;
                                }
                        }

                        continue;
                } else if (cur->size > num_units) {
                        /* Add the desired new block to the end of the oversized block */
                        cur->size -= num_units;
                        cur += cur->size;
                        cur->size = num_units;
                } else {
                        /* Extract the current block from the list and join surrounding blocks */
                        prev->next = cur->next;
                }

                return cur + 1;
        }

        return NULL;
}

/*
  Scan a contiguous memory region for pointers and tag corresponding blocks curerntly in use
*/
static void scan_region(void *start, void *end) {
        void *cur; /* Current address in memory being examined */
        void *memval; /* Pointer read from value in memory */
        struct header *block; /* Current block in memory */

        /* Iterate over all addresses, aligned to pointer size */
        for (cur = start; cur < end; cur++) {
                block = usedp;
                memval = *(void**)cur;

                /* Iterate through blocks to identify and tag the block an address is from */
                do {
                        if (block + 1 <= memval && memval < block + block->size + 1) {
                                block->next = block->next | 1;
                                break;
                        }
                } while ((block = UNTAG(bp->next)) != usedp);
        }
}

/*
  Scan the heap (consisting of the list of usedp) for references to blocks in use
  and tag them as such
*/
static void scan_heap(void) {
        void *cur;
        void *memval;
        struct header *block, *search;

        /* Iterate over all blocks in the used block list */
        for (block = UNTAG(usedp->next); block != usedp; block = UNTAG(block->next)) {
                /* Skip block if it has already been considered and marked as not-in-use */
                if (block->next & 1 == 0) {
                        continue;
                }

                /* Iterate over words in block to find any references to blocks to be freed */
                for (cur = block + 1; cur < block + block->size + 1; cur++) {
                        /* Read a potential address from memory */
                        memval = *(void**)cur;

                        /* Identify the block a memory address belongs to and tag it as in-use */
                        for (search = block->next; UNTAG(search) != block; search = UNTAG(search->next)) {
                                if (search != block &&
                                    search + 1 <= memval && memval <= search + search->size + 1) {
                                        search->next = TAG(search->next, 1);
                                        break;
                                }
                        }
                }
        }
}

/*
  Find the address of the stack's beginning and initialize variables
*/
void dumpster_init(void)
{
        FILE *fp;

        if (inititalized) {
                return;
        }

        /* Read stack address from /proc */
        fp = fopen("/proc/self/stat", "r");
        if (fp == NULL) {
                return;
        }
        fscanf(fp,
               "%*d %*s %*c %*d %*d %*d %*d %*d %*u "
               "%*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld "
               "%*ld %*ld %*ld %*ld %*llu %*lu %*ld "
               "%*lu %*lu %*lu %lu",
               &stack_bottom);

        fclose(fp);

        /* Initialize "empty" linked list of used blocks */
        usedp = NULL;

        /* Initialize free linked list as a circular empty linked list */
        base.next = freep = &base;
        base.size = 0;
}

/*
  Identify orphaned memory blocks and free them
*/
void dumpster_collect(void) {
        struct header *cur, *prev, *tmp;
        extern char end, etext;

        /* No memory has been allocated */
        if (usedp == NULL) {
                return;
        }

        /* Scan data segment */
        scan_region(&etext, &end);


        /* Move address of EBP into stack_top */
        asm volatile ("movl %%ebp, %0" : "=r" (stack_top));

        /* Scan stack */
        scan_region(stack_top, stack_bottom);

        /* Scan heap */
        scan_heap();

        prev = usedp;
        cur = UNTAG(usedp->next);

        /* Stop when all nodes have been searched */
        while (cur != usedp) {
                if (TAGOF(cur->next) != 1) {
                        /* Free a block and reconnect the surrounding linked list */
                        tmp = cur;
                        cur = UNTAG(cur->next);
                        add_to_free(tmp);

                        /* Freed final block in the used list */
                        if (usedp == tmp) {
                                usedp = NULL;
                                break;
                        }

                        prev->next = p | TAGOF(prev->next);
                } else {
                        p->next = UNTAG(p);
                }
        }
}

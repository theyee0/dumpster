#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include <errno.h>

#define PAGE_SIZE 4096
#define MAX_DELAY 500

/* Memory page item */
struct header {
        unsigned int size;
        struct header *next;
};

/* Circular linked lists representing memory blocks */
static struct header base;
static struct header *freep = &base;
static struct header *usedp = NULL;

/* Tags representing the usage state of different nodes */
enum tags {
        WHITE = 0x0,
        BLACK = 0x1,
        GREY = 0x2
};

/* Structure for linked list of memory blocks to be searched */
struct color_node {
        struct header *p;
        struct color_node *next;
};

/* State data */
static int initialized = 0;
static int collecting = 0;
struct timespec cur_time;

/* Location of stack */
static void *stack_base;
static void *stack_top;

/* Tri-color marking algorithm */
static struct color_node *grey_list = NULL;
static struct color_node *black_list = NULL;


/* Given a pointer, return a tagged pointer */
static void *tag(void *ptr, unsigned long long int tag) {
        unsigned long long int ptrl = (unsigned long long int)ptr;

        return (void*)((ptrl & (~0x3)) | (tag & 0x3));
}

/* Given a tagged pointer, return the actual memory address */
static void *untag(void *ptr) {
        unsigned long long int ptrl = (unsigned long long int)ptr;

        return (void*)(ptrl & (~0x3));
}

/* Given a tagged pointer, return the value of the tag */
static unsigned long long int tagof(void *ptr) {
        unsigned long long int ptrl = (unsigned long long int)ptr;

        return ptrl & 0x3;
}

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
        void *p; /* Pointer to location where new block will be added */
        struct header *new_block; /* Pointer to new block */

        /* Force the allocation to be at least the page size */
        if (num_units < PAGE_SIZE) {
                num_units = PAGE_SIZE / sizeof(struct header);
        }

        /* Attempt to allocate new memory and ensure it doesn't fail */
        if ((p = mmap(NULL,
                      num_units * sizeof(struct header),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS,
                      -1,
                      0)) == MAP_FAILED) {
                perror("morecore()");
                return NULL;
        }

        /* Update pointer at end to reflect new memory */
        new_block = (struct header*)p;
        new_block->size = num_units;
        add_to_free(new_block);

        return freep;
}

/*
  Allocate a new block of size at least `alloc_size` and return a pointer
*/
void *dumpster_alloc(size_t alloc_size) {
        size_t units;
        struct header *cur, *prev;

        /* Align units of allocation to header size, plus one for an actual header */
        units = (alloc_size + sizeof(struct header) - 1) / sizeof(struct header) + 1;

        /* Iterate over free blocks to try to find an existing free block */
        for (prev = freep, cur = untag(prev->next);; prev = cur, cur = untag(prev->next)) {
                if (cur->size < units) {
                        if (cur == freep) {
                                /* If the attempt to find a free block failed, allocate a new one */
                                cur = morecore(units);
                                if (cur == NULL) {
                                        return NULL;
                                }
                        }

                        continue;
                } else if (cur->size > units) {
                        /* Add the desired new block to the end of the oversized block */
                        cur->size -= units;
                        cur += cur->size;
                        cur->size = units - 1;
                } else {
                        /* Extract the current block from the list and join surrounding blocks */
                        prev->next = cur->next;
                }

                if (usedp == NULL) {
                        cur->next = cur;
                        usedp = cur;
                } else {
                        cur->next = usedp->next;
                        usedp->next = cur;
                }

                return cur + 1;
        }

        return NULL;
}

/*
  Given a linked list of memory blocks and a memory address, find the block containing that
  memory address and tag it with a specified color
*/
static void tag_unclean_block(struct header *list, void* memval, enum tags color_tag) {
        struct header *cur = list;

        /* No memory blocks to tag */
        if (list == NULL) {
                return;
        }

        do {
                /* Tag the block if the memory address is between its bounds */
                if (list != cur &&
                    (void*)(cur + 1) <= memval &&
                    memval <= (void*)(cur + cur->size + 1)) {
                        cur->next = tag(cur->next, color_tag);
                        break;
                }
        } while ((cur = untag(cur->next)) != list);
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
                tag_unclean_block(usedp, memval, BLACK);
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
        for (block = untag(usedp->next); block != usedp; block = untag(block->next)) {
                /* Skip block if it has already been considered and marked as not-in-use */
                if (tagof(block->next) == 0) {
                        continue;
                }

                /* Iterate over words in block to find any references to blocks to be freed */
                for (cur = block + 1; cur < (void*)(block + block->size + 1); cur++) {
                        /* Read a potential address from memory */
                        memval = *(void**)cur;

                        /* Identify the block a memory address belongs to and tag it as in-use */
                        tag_unclean_block(usedp, memval, BLACK);
                }
        }
}

/*
  Find the address of the stack's beginning and initialize variables
*/
void dumpster_init(void)
{
        FILE *fp;

        if (initialized) {
                return;
        }

        /* Read stack address from /proc */
        if ((fp = fopen("/proc/self/stat", "r")) == NULL) {
                return;
        }

        fscanf(fp,
               "%*d %*s %*c %*d %*d %*d %*d %*d %*u "
               "%*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld "
               "%*ld %*ld %*ld %*ld %*llu %*lu %*ld "
               "%*lu %*lu %*lu %lu",
               &stack_base);

        fclose(fp);

        initialized = 1;

        /* Initialize "empty" linked list of used blocks */
        usedp = NULL;

        /* Initialize free linked list as a circular empty linked list */
        base.next = freep = &base;
        base.size = 0;
}

/*
  Identify orphaned memory blocks and free them using "Mark and Sweep"
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

        /* Move address of RBP into stack_top */
        asm volatile ("mov %%rbp, %0" : "=r" (stack_top));

        /* Scan stack */
        scan_region(stack_top, stack_base);

        /* Scan heap */
        scan_heap();

        prev = usedp;
        cur = untag(usedp->next);

        /* Stop when all nodes have been searched */
        while (cur != usedp) {
                if (tagof(cur->next) == WHITE) {
                        /* Free a block and reconnect the surrounding linked list */
                        tmp = cur;
                        cur = untag(cur->next);
                        add_to_free(tmp);

                        /* Freed final block in the used list */
                        if (usedp == tmp) {
                                usedp = NULL;
                                break;
                        }

                        prev->next = tag(cur, tagof(prev->next));
                } else {
                        cur = untag(cur->next);
                }
        }
}

/*
  Given a list of memory blocks and a memory address, find the corresponding memory block
  and tag it and its descendants.

  Check that the time limit has not been exceeded while doing so, otherwise return early.
*/
static void tag_unclean_block_incremental(struct header *list,
                                          void* memval,
                                          struct color_node *new_list,
                                          enum tags color_tag,
                                          struct timespec start_time) {
        struct header *block = list;
        struct color_node *tmp;

        /* Iterate over blocks and find the block the address lies between */
        do {
                if ((void*)(block + 1) <= memval &&
                    memval <= (void*)(block + block->size + 1)) {
                        block->next = tag(block->next, color_tag);

                        /* Allocate a new element and push it into the linked list of grey blocks */
                        tmp = malloc(sizeof(*tmp));
                        tmp->p = block->next;
                        tmp->next = new_list;
                        grey_list = tmp;
                        break;
                }

                /* Check to ensure tha tthe time limit has not been exceeded */
                clock_gettime(CLOCK_REALTIME, &cur_time);

                if (cur_time.tv_nsec - start_time.tv_nsec >= MAX_DELAY) {
                        return;
                }
        } while ((block = untag(block->next)) != list);
}

/*
  Scan a contiguous memory region for pointers and tag corresponding blocks curerntly in use
*/
static void scan_region_incremental(void *start, void *end, struct timespec start_time) {
        void *cur; /* Current address in memory being examined */
        void *memval; /* Pointer read from value in memory */
        struct header *block; /* Current block in memory */

        /* Iterate over all addresses, aligned to pointer size */
        for (cur = start; cur < end; cur++) {
                memval = *(void**)cur;

                /* Iterate through blocks to identify and tag the block an address is from */
                tag_unclean_block_incremental(usedp, memval, grey_list, GREY, start_time);

                /* Return early if required */
                clock_gettime(CLOCK_REALTIME, &cur_time);

                if (cur_time.tv_nsec - start_time.tv_nsec >= MAX_DELAY) {
                        return;
                }
        }
}

/*
  Scan the heap (consisting of the list of usedp) for references to blocks in use
  and tag them as such
*/
static void scan_heap_incremental(struct timespec start_time) {
        void *cur;
        void *memval;
        struct header *block, *search;
        struct color_node *tmp;

        /* Iterate over all blocks in the used block list */
        while (grey_list != NULL) {
                /* Skip block if it has already been considered and marked as not-in-use */
                block = untag(grey_list->p);

                if (tagof(block->next) != BLACK) {
                        /* Move to the next element in the stack to search */
                        tmp = grey_list->next;
                        free(grey_list);
                        grey_list = tmp;
                        continue;
                }

                /* Iterate over words in block to find any references to blocks to be freed */
                for (cur = block + 1; cur < (void*)(block + block->size + 1); cur++) {
                        /* Read a potential address from memory */
                        memval = *(void**)cur;

                        /* Identify the block a memory address belongs to and tag it as in-use */
                        tag_unclean_block_incremental(cur, memval, grey_list, GREY, start_time);
                }

                /* Tag the current block once all of its descendants have been searched */
                block->next = tag(block->next, BLACK);
                tmp = malloc(sizeof(*tmp));
                tmp->p = block;
                tmp->next = black_list;
                black_list = tmp;

                /* Move to the next element in the stack to search */
                tmp = grey_list->next;
                free(grey_list);
                grey_list = tmp;
        }
}

/*
  Incrementally collect memory, obeying a hard time bound for each attempted free.

  If the time bound is exceeded, terminate the search.

  Future function calls will refresh the entries to account for shifting memory, then
  proceed where the previous call left off. If the previous call was completed, then this is
  equivalent to beginning a fresh mark and sweep cycle.
*/
void dumpster_collect_incremental() {
        struct header *cur, *prev, *tmp;
        extern char end, etext;
        struct timespec start_time;

        /* No memory has been allocated */
        if (usedp == NULL) {
                return;
        }

        /* Initialize all blocks to be searched if it's a true new collection cycle */
        if (!collecting) {
                prev = usedp;

                for (cur = untag(usedp->next); cur != usedp; cur = untag(cur->next)) {
                        prev->next = tag(cur, WHITE);
                }

                collecting = 1;
        }

        /* Set the start time for reference */
        clock_gettime(CLOCK_REALTIME, &start_time);
        
        /* Scan data segment */
        scan_region_incremental(&etext, &end, start_time);
        clock_gettime(CLOCK_REALTIME, &cur_time);

        if (cur_time.tv_nsec - start_time.tv_nsec >= MAX_DELAY) {
                return;
        }
        /* Move address of RBP into stack_top */
        asm volatile ("mov %%rbp, %0" : "=r" (stack_top));

        /* Scan stack */
        scan_region_incremental(stack_top, stack_base, start_time);
        clock_gettime(CLOCK_REALTIME, &cur_time);

        if (cur_time.tv_nsec - start_time.tv_nsec >= MAX_DELAY) {
                return;
        }

        /* Scan heap */
        scan_heap_incremental(start_time);
        clock_gettime(CLOCK_REALTIME, &cur_time);

        if (cur_time.tv_nsec - start_time.tv_nsec >= MAX_DELAY) {
                return;
        }

        prev = usedp;
        cur = untag(usedp->next);

        /* Stop freeing memory blocks when all nodes have been searched */
        while (cur != usedp) {
                if (tagof(cur->next) == WHITE) {
                        /* Free a block and reconnect the surrounding linked list */
                        tmp = cur;
                        cur = untag(cur->next);
                        add_to_free(tmp);

                        /* Freed final block in the used list */
                        if (usedp == tmp) {
                                usedp = NULL;
                                break;
                        }

                        prev->next = tag(cur, tagof(prev->next));
                } else {
                        cur->next = untag(cur);
                }
        }

        collecting = 0;
}

/* Compute the fraction of memory that is fragmented between used blocks */
double compute_fragmentation() {
        struct header *cur = freep;
        unsigned long long int available = 0;
        unsigned long long int fragmented = 0;

        /* Compute total available and fragmented memory */
        do {
                available += cur->size * sizeof(struct header);
                fragmented += cur->next - (cur + cur->size);

                cur = untag(cur->next);
        } while (cur != freep);

        return (double)fragmented / (available + fragmented);
}

/*
  Print statistics regarding how much memory is used, what size the memory blocks are, and
  what type of memory (free, used)
*/
double print_statistics(int verbose) {
        struct header *cur = freep;
        unsigned long long int free_memory = 0;
        unsigned long long int used_memory = 0;

        if (cur != NULL) {
                printf("--- Free Blocks ---\n");

                if (verbose) {
                        printf("Free block sizes:");
                }

                do {
                        if (verbose) {
                                printf(" (%p, %d)", (void*)cur, cur->size * sizeof(struct header));
                        }

                        free_memory += cur->size * sizeof(struct header);

                        cur = untag(cur->next);
                } while (cur != freep);

                if (verbose) {
                        printf("\n");
                }

                printf("Free: %dB\n\n", free_memory);
        } else {
                printf("--- There are no free blocks of memory currently on standby. ---\n\n");
        }

        cur = usedp;

        if (cur != NULL) {
                printf("--- Used Blocks ---\n");

                if (verbose) {
                        printf("Used block sizes:");
                }

                do {
                        if (verbose) {
                                printf(" (%p, %d)", (void*)cur, cur->size * sizeof(struct header));
                        }

                        used_memory += cur->size * sizeof(struct header);

                        cur = untag(cur->next);
                } while (cur != usedp);

                if (verbose) {
                        printf("\n");
                }

                printf("Used: %dB\n\n", used_memory);
        } else {
                printf("--- No memory is currently in use. ---\n\n");
        }

        return (double)free_memory / (free_memory + used_memory);
}

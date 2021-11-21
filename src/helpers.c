#include "helpers.h"
#include "debug.h"
#include "icsmm.h"

/* 
* Finds the next fit block in the freelist, starting from freelist_next
* @return pointer to the block 
*/
size_t adjust_size(size_t size){ 
    if(size <= DSIZE){
        return MIN_BLOCK_SIZE; 
    }
    else{
        return DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);
    }
}

/*
 * Traverse through the freelist starting from freelist_next 
 * to find a block that has a greater or equal size to the 
 * adjusted size of the requested block
 * Return NULL if no fit is found or list is empty
 */
void* find_next_fit(size_t asize){
    ics_free_header* current = freelist_next;
    if(freelist_next == NULL){
        /* Empty freelsit */
        return NULL;
    }
    uint64_t bsize; // block_size of the block that current is pointing to
    while(1){
        bsize = current->header.block_size;
        if(bsize >= asize){
            return (void*)current;
        }

        if(current->next == NULL){
            /* Wrap around to head */
            current = freelist_head;
        }
        else{
            current = current->next;
        }

        if(current == freelist_next){
            /* THERE IS NO FIT */
            return NULL;
        }
    }
}

/*
* Place the requested block at the beginning of the free block,
* splitting only if the size of the remaining would equal or exceed
* the minumum block size (32 bytes)
*/
void* allocate(void* bp, size_t asize, size_t reqSize){
    ics_free_header* block = (ics_free_header*)bp;
    uint64_t bsize = block->header.block_size;
    // free: 64 
    // malloc 9 -> 32
    // 64 - 32 = 31
    if((bsize - asize) >= MIN_BLOCK_SIZE){ /* SPLIT */
        /* Update allocated block header */
        block->header.block_size     = SET_ALLOC_BIT(asize,1);
        block->header.requested_size = reqSize;

        /* Locate & update allocated block footer */
        ics_footer* footer            = (void*)block + asize - WSIZE;
        footer->block_size            = SET_ALLOC_BIT(asize,1);
        footer->fid                   = FOOTER_MAGIC;
        footer->requested_size        = reqSize;

        /* Locate & update new free block header */
        uint64_t new_block_size                = bsize - asize;
        ics_free_header* new_free_header       = bp + asize; // locate new postition for the splitted block
        new_free_header->header.block_size     = new_block_size;
        new_free_header->header.hid            = HEADER_MAGIC;
        new_free_header->header.requested_size = 0;

        /* Update freelist */
        if(block == freelist_head){
            freelist_head = new_free_header;
        }
        freelist_next = new_free_header;
        if(block->prev != NULL){
            block->prev->next = new_free_header;
        }
        if(block->next != NULL){
            block->next->prev = new_free_header;
        }
        new_free_header->next = block->next;
        new_free_header->prev = block->prev;

        /* Locate & update new free block footer */
        // technically no need to assign fid and req size again (it's already there)
        ics_footer* new_free_footer     = (void*)new_free_header + new_block_size - WSIZE;
        new_free_footer->block_size     = new_block_size;
        new_free_footer->fid            = FOOTER_MAGIC; 
        new_free_footer->requested_size = 0; 

        /* Return the pointer to the block */
        return (void*)block;

    } else { /* NO SPLIT */
        /* Update header */
        block->header.block_size     = SET_ALLOC_BIT(bsize,1);
        block->header.requested_size = reqSize;

        /* Update freelist next/prev */
        if(block->prev != NULL){
            block->prev->next = block->next;
        }
        if(block->next != NULL){
            block->next->prev = block->prev;
        }

        /* Freelist_head */
        if(block->next == NULL && block == freelist_head){
            freelist_head = NULL;
        } 
        else if(block->next != NULL && block == freelist_head){
            freelist_head = block->next;
        }

        /* Freelist_next */
        if(block->next == NULL){
            freelist_next = freelist_head;
        }
        else{
            freelist_next = block->next;
        }

        /* Update footer */
        ics_footer* footer            = (void*)block + bsize - WSIZE;
        footer->block_size            = SET_ALLOC_BIT(bsize,1);
        footer->requested_size        = reqSize;

        /* Return the pointer to the block */
        return (void*)block;
    }
}

/*
 * To determine if a ptr is not valid, (i) the header and footer are in
 * the managed  heap space, (ii) check the hid field of the ptr's header for
 * special value (iii) check the fid field of the ptr's footer for special value,
 * (iv) check that the block_size in the ptr's header and footer are equal, (v) 
 * the allocated bit is set in both ptr's header and footer, and (vi) the 
 * requested_size is identical in the header and footer.
 */
int validate_address(void* ptr){
    /* Header and footer are in the managed heap space */
    void* epilogue = ics_get_brk() - WSIZE;
    if(GET_HDR(ptr) >= epilogue || GET_FTR(ptr) >= epilogue || GET_HDR(ptr) <= prologue || GET_FTR(ptr) <= prologue ){
        return -1;
    }

    /* Correct HID and FID fields */
    if(((ics_header*)GET_HDR(ptr))->hid != HEADER_MAGIC ||
       ((ics_footer*)GET_FTR(ptr))->fid != FOOTER_MAGIC){
        return -1;
    }

    /* Same header and footer block_size */
    if(((ics_header*)GET_HDR(ptr))->block_size != ((ics_footer*)GET_FTR(ptr))->block_size){
        return -1;
    }

    /* Header and footer are marked as allocated */
    size_t hdr_alloc = ((ics_header*)GET_HDR(ptr))->block_size & 0x1;
    size_t ftr_alloc = ((ics_footer*)GET_FTR(ptr))->block_size & 0x1; 
    if(!hdr_alloc || !ftr_alloc){
        return -1;
    }

    /* Same header and footer requested_size */
    if(((ics_header*)GET_HDR(ptr))->requested_size != ((ics_footer*)GET_FTR(ptr))->requested_size){
        return -1;
    }

    return 0;
}

/* 
 * Put the block into the freelist (address-ordered)
 */
void insertFreeList(void* fh){
    ics_free_header* freeheader = (ics_free_header*)fh;

    /* Freelist is empty, this becomes the head */
    if(freelist_head == NULL){
        printf("    insertFreeList::list is empty, putting into front\n");
        freelist_head = freeheader;
        freelist_next = freelist_head;
        freeheader->prev = NULL;
        freeheader->next = NULL;
        return;
    }

    ics_free_header* current = freelist_head;
    while(1){
        if(freeheader < current){
            freeheader->prev = current->prev;
            freeheader->next = current;
            if(current->prev != NULL){
                current->prev->next = freeheader;
            }
            current->prev = freeheader;
            if(current == freelist_head){
                freelist_head = freeheader;
            }
            return;
        }
        else if(current->next == NULL){
            break;
        }
        current = current->next;
    }
    /* Inserted in the back */
    current->next = freeheader;
    freeheader->prev = current;
    freeheader->next = NULL;
}

void coalesce(void* block){
    /* Get alloc bits from prev and next blocks */
    uint64_t prev_alloc = ((ics_footer*)(GET_HDR(block) - WSIZE))->block_size & 0x1;
    uint64_t next_alloc = ((ics_header*)(GET_FTR(block) + WSIZE))->block_size & 0x1;
    uint64_t size = ((ics_header*)GET_HDR(block))->block_size;

    /* CASE 4: If both prev and next are free */
    if(!prev_alloc && !next_alloc){
        uint64_t prev_size = ((ics_footer*)(GET_HDR(block) - WSIZE))->block_size;
        ics_free_header* prev_free_block = GET_HDR(block) - prev_size;
        uint64_t next_size = ((ics_header*)(GET_FTR(block) + WSIZE))->block_size;
        ics_free_header* next_free_block = (ics_free_header*)(GET_FTR(block) + WSIZE);

        /* Update the header */
        size += prev_size + next_size;
        prev_free_block->header.block_size = size;

        /* Update the footer */
        ((ics_footer*)GET_FTR(prev_free_block))->block_size = size;

        /* Update the freelist & freelist_next */
        if(freelist_next == next_free_block){
            freelist_next = prev_free_block;
        }
        prev_free_block->next = next_free_block->next;
        if(next_free_block->next != NULL){
            next_free_block->next->prev = prev_free_block;
        }

    }
    /* CASE 2: If prev is free and next is allocated */
    else if(!prev_alloc && next_alloc){
        /* Calculate new block size */
        printf("    coalesce::CASE 2\n");
        uint64_t prev_free_size = ((ics_footer*)(GET_HDR(block) - WSIZE))->block_size;
        size += prev_free_size;
        printf("    coalesce::aggregated size is %ld\n", size);

        /* Update the header */
        ics_free_header* prev_free_block = GET_HDR(block) - prev_free_size;
        prev_free_block->header.block_size = size;

        /* Update the footer */
        ((ics_footer*)GET_FTR(block))->block_size = size;
    }

    /* CASE 3: If prev is allocated and next is free */
    else if(prev_alloc && !next_alloc){
        /* Update header block size */
        ics_free_header* next_block = (ics_free_header*)(GET_FTR(block) + WSIZE);
        size += next_block->header.block_size;
        ((ics_header*)GET_HDR(block))->block_size = size;

        /* Update footer block size */
        ((ics_footer*)GET_FTR(block))->block_size = size;

        /* Put the new coalesced block into freelist */
        ((ics_free_header*)GET_HDR(block))->next = next_block->next;
        ((ics_free_header*)GET_HDR(block))->prev = next_block->prev;
        if(next_block->next != NULL){
            next_block->next->prev = GET_HDR(block);
        }
        if(next_block->prev != NULL){
            next_block->prev->next = GET_HDR(block);
        }
        if(next_block == freelist_head){
            freelist_head = GET_HDR(block);
        }
    }

    /* CASE 1 : If prev block and next block are both allocated, no coalesce */
    else { 
        ics_free_header* new_free_header = (ics_free_header*)GET_HDR(block);
        insertFreeList((void*)new_free_header);
    }
}

void* extend_heap(size_t asize){
    printf("    extend_heap::inside extend_heap...\n");
    void* brk;
    while(1){
        brk = ics_inc_brk();
        if(*(int*)brk == -1){
            return NULL;
        }
        printf("    extend_heap::inc brk point...\n");
        /* Locate e pilogue */
        void* epilogue = ics_get_brk() - WSIZE;
        ((ics_header*)epilogue)->block_size = 1;
        ((ics_header*)epilogue)->hid = HEADER_MAGIC;
        ((ics_header*)epilogue)->requested_size = 1;
        printf("    extend_heap::set epilogue...\n");

        /* Locate footer */
        ics_footer* new_footer = epilogue - WSIZE;
        new_footer->block_size = PAGE;
        new_footer->fid = FOOTER_MAGIC;
        new_footer->requested_size = 0;
        printf("    extend_heap::set footer...\n");

        /* Locate header (epilogue turns into header) */
        ics_free_header* new_free_header = brk - WSIZE;
        new_free_header->header.block_size = PAGE;
        new_free_header->header.hid = HEADER_MAGIC;
        new_free_header->header.requested_size = 0;
        printf("    extend_heap::set header (not next/prev)...\n");

        /* Coalesce & update freelist */
        coalesce(PAYLOAD((void*)new_free_header));
        printf("    extend_heap::coalesced...\n");

        /* Find_next_fit, break if found */
        void* fit; 
        // printf("    extend_heap::looking for the fit...\n");
        if((fit = find_next_fit(asize)) != NULL){
            return fit;
        }
    }
}

/*
 * Copy bytes_to_copy amount of bytes from ptr's payload
 * to block's payload
 */
void copy_payload(void* ptr, void* block, size_t bytes_to_copy){
    int i; 
    for(i = 0; i < bytes_to_copy; i++){
        *(char*)(PAYLOAD(block)+i) = *(char*)(ptr+i);
    }
}
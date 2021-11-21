#include "icsmm.h"
#include "debug.h"
#include "helpers.h"
#include <stdio.h>
#include <stdlib.h>

int first_malloc = 1;
void* prologue;
/*
 * The allocator MUST store the head of its free list in this variable. 
 * Doing so will make it accessible via the extern keyword.
 * This will allow ics_freelist_print to access the value from a different file.
 */
ics_free_header *freelist_head = NULL;

/*
 * The allocator MUST use this pointer to refer to the position in the free list to 
 * starting searching from. 
 */
ics_free_header *freelist_next = NULL;

/*
 * This is your implementation of malloc. It acquires uninitialized memory from  
 * ics_inc_brk() that is 16-byte aligned, as needed.
 *
 * @param size The number of bytes requested to be allocated.
 *
 * @return If successful, the pointer to a valid region of memory of at least the
 * requested size is returned. Otherwise, NULL is returned and errno is set to 
 * ENOMEM - representing failure to allocate space for the request.
 * 
 * If size is 0, then NULL is returned and errno is set to EINVAL - representing
 * an invalid request.
 */
void *ics_malloc(size_t size) { 
    size_t asize; /* Adjusted block size */
    void* block;

    /* INVALID REQUESTS */
    if(size == 0){
        errno = EINVAL;
        return NULL;
    }
    if(size > MAXHEAP_SIZE){
        errno = ENOMEM;
        return NULL;
    }

    /* FIRST MALLOC - SETUP */
    if(first_malloc){
        /* Allocate first page */
        void* start_addr = ics_inc_brk();
        if(*(int*)start_addr == -1){
            errno = ENOMEM;
            return NULL;
        }
        first_malloc = 0;
        prologue = start_addr;

        /* Set Prologue */
        ((ics_footer*)start_addr)->block_size = 1;
        ((ics_footer*)start_addr)->fid = FOOTER_MAGIC;
        ((ics_footer*)start_addr)->requested_size = 1;

        /* Set Epilogue */
        void* end_addr = ics_get_brk() - WSIZE;
        ((ics_header*)end_addr)->block_size = 1;
        ((ics_header*)end_addr)->hid = HEADER_MAGIC;
        ((ics_header*)end_addr)->requested_size = 1;

        /* Create first free block's header*/
        void* first_block_header = start_addr + WSIZE;
        ((ics_header*)first_block_header)->block_size = PAGE - DSIZE; // 4096 - 16
        ((ics_header*)first_block_header)->hid = HEADER_MAGIC;
        ((ics_header*)first_block_header)->requested_size = 0;
        ((ics_free_header*)first_block_header)->next = NULL;
        ((ics_free_header*)first_block_header)->prev = NULL;

        /* Update freelist pointers */
        freelist_head = first_block_header;
        freelist_next = freelist_head;

        /* Create first free block's footer*/
        void* first_block_footer = end_addr - WSIZE;
        ((ics_footer*)first_block_footer)->block_size = PAGE - DSIZE;
        ((ics_footer*)first_block_footer)->fid = FOOTER_MAGIC;
        ((ics_footer*)first_block_footer)->requested_size = 0;
    }
 
    /* Adjust block size to include overhead and alignment reqs */
    asize = adjust_size(size);

    /* Search the free list for the next fit */
    if((block = find_next_fit(asize)) != NULL){
        allocate(block, asize, size); /* Allocate the block & split (if necessary) */
        return PAYLOAD(block); /* Return the pointer to the payload */
    }

    /* No fit found. Get more memory and place the block */
    if((block = extend_heap(asize)) == NULL){
        errno = ENOMEM;
        return NULL;
    }
    allocate(block, asize, size);
    return PAYLOAD(block);

}

/*
 * Marks a dynamically allocated block as no longer in use and coalesces with 
 * adjacent free blocks (as specified by Homework Document). 
 * Adds the block to the appropriate bucket according to the block placement policy.
 *
 * @param ptr Address of dynamically allocated memory returned by the function
 * ics_malloc.
 * 
 * @return 0 upon success, -1 if error and set errno accordingly.
 * 
 * If the address of the memory being freed is not valid, this function sets errno
 * to EINVAL. To determine if a ptr is not valid, (i) the header and footer are in
 * the managed  heap space, (ii) check the hid field of the ptr's header for
 * special value (iii) check the fid field of the ptr's footer for special value,
 * (iv) check that the block_size in the ptr's header and footer are equal, (v) 
 * the allocated bit is set in both ptr's header and footer, and (vi) the 
 * requested_size is identical in the header and footer.
 */
int ics_free(void *ptr) { 
    /* Validate the address of the memory */
    if(validate_address(ptr) == -1){
        // printf("    ics_free::not valid address\n");
        errno = EINVAL;
        return -1;
    }

    /* Update this block's header & footer as free */
    size_t free_size = ((ics_header*)GET_HDR(ptr))->block_size;
    ((ics_header*)GET_HDR(ptr))->block_size = free_size - 0x1;
    ((ics_footer*)GET_FTR(ptr))->block_size = free_size - 0x1;
    ((ics_header*)GET_HDR(ptr))->requested_size = 0;
    ((ics_footer*)GET_FTR(ptr))->requested_size = 0;

    /* Coalesce */
    coalesce(ptr);
    
    return 0;
}

/*
 * Resizes the dynamically allocated memory, pointed to by ptr, to at least size 
 * bytes. See Homework Document for specific description.
 *
 * @param ptr Address of the previously allocated memory region.
 * @param size The minimum size to resize the allocated memory to.
 * @return If successful, the pointer to the block of allocated memory is
 * returned. Else, NULL is returned and errno is set appropriately.
 *
 * If there is no memory available ics_malloc will set errno to ENOMEM. 
 *
 * If ics_realloc is called with an invalid pointer, set errno to EINVAL. See ics_free
 * for more details.
 *
 * If ics_realloc is called with a valid pointer and a size of 0, the allocated     
 * block is free'd and return NULL.
 */
void *ics_realloc(void *ptr, size_t size) {
    /* Validate ptr's address */
    if(validate_address(ptr) == -1){
        errno = EINVAL;
        return NULL;
    }

    /* Valid address with size 0 */
    if(size == 0){
        /* Free ptr and return NULL */
        ics_free(ptr);
        return NULL;
    }

    /* Find a new free block that matches the newly requested size (if no space, error) */
    void* block;
    ics_header* old_header = (ics_header*)GET_HDR(ptr);
    size_t old_req = old_header->requested_size;
    size_t asize = adjust_size(size);
    size_t bytes_to_copy = MIN(old_req, size);

    if((block = find_next_fit(asize)) != NULL){
        allocate(block, asize, size);
        /* Copy MIN(old_req, new_req) amount of information from old block to new block */
        copy_payload(ptr, block, bytes_to_copy);
        /* Free the old block */ 
        ics_free(ptr);
        return PAYLOAD(block);
    }

    /* Not enough space, extend heap */
    if((block = extend_heap(asize)) == NULL){
        errno = ENOMEM;
        return NULL;
    }
    allocate(block, asize, size);
    copy_payload(ptr, block, bytes_to_copy);
    ics_free(ptr);
    return PAYLOAD(block);
}
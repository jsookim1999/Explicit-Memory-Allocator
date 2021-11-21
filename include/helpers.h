#ifndef HELPERS_H
#define HELPERS_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Helper function declarations go here */
#define MAXHEAP_SIZE 20448
#define MIN_BLOCK_SIZE 32
#define PAGE 4096
#define WSIZE 8
#define DSIZE 16
#define MIN(x,y) ((x) > (y) ?  (y) : (x))
#define SET_ALLOC_BIT(size, alloc) ((size) | (alloc))
#define PAYLOAD(ptr) ((ptr) + WSIZE) // Get payload from the pointer to the header
#define GET_SIZE(ptr) (((ics_header*)(ptr))->block_size & ~0x7)
#define GET_HDR(ptr) ((void*)(ptr) - WSIZE)
#define GET_FTR(ptr) ((void*)(ptr) + GET_SIZE(GET_HDR(ptr)) - DSIZE)

extern void* prologue;

size_t adjust_size(size_t size);
void* find_next_fit(size_t asize);
void* allocate(void* bp, size_t asize, size_t reqSize);
int validate_address(void* ptr);
void coalesce(void* block);
void* extend_heap(size_t asize);
void copy_payload(void* ptr, void* block, size_t bytes_to_copy);
#endif

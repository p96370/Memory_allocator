// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"

int heap_prealloc = 1;
struct block_meta *global_base;

// get the last block from the list
struct block_meta *get_last_block(struct block_meta *head)
{
	if (head == NULL)
		return NULL;

	if (head && !head->next)
		return head;

	while (head && head->next)
		head = head->next;

	return head;
}

// add another block to the list
struct block_meta *request_space(struct block_meta *head, size_t size, size_t mmap_threshold)
{
	struct block_meta *new_block;
	size_t block_size = ALIGN(META_SIZE) + ALIGN(size);

	if (heap_prealloc == 0 && block_size < mmap_threshold) {
		head = get_last_block(head);
		// last block of list is free, so we allocate only the difference between given size
		// and the size of the last block
		if (head->status == STATUS_FREE) {
			new_block = sbrk(ALIGN(size) - head->size);
			DIE(new_block == MAP_FAILED, "sbrk failed");
			head->size = ALIGN(size);
			head->status = STATUS_ALLOC;
			return head;
		}

		// last block is not free, so we allocate a block of given size
		new_block = sbrk(block_size);
		new_block->size = ALIGN(size);

		DIE(new_block == MAP_FAILED, "sbrk failed");
		new_block->status = STATUS_ALLOC;

		new_block->next = NULL;
		head->next = new_block;
		return new_block;

	} else {
		// if it is the first time an allocation takes place we must allocate the given threshold
		if (heap_prealloc == 1 && block_size < mmap_threshold) {
			new_block = sbrk(MMAP_THRESHOLD);
			DIE(new_block == MAP_FAILED, "sbrk failed");
			new_block->status = STATUS_ALLOC;

			head = get_last_block(head);
			new_block->next = NULL;
			if (head)
				head->next = new_block;
			new_block->size = ALIGN(size);

			return new_block;
		}

		// given size if bigger than our threshold, so we map the block at the end of the list
		new_block = mmap(NULL, block_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		DIE(new_block == MAP_FAILED, "mmap failed");
		new_block->status = STATUS_MAPPED;

		head = get_last_block(head);
		new_block->next = NULL;
		if (head)
			head->next = new_block;
		new_block->size = ALIGN(size);

		return new_block;
	}
}

// split one block into two, marking the second part as free
struct block_meta *split_block(struct block_meta *block, size_t size)
{
	struct block_meta *new_block = (void *)block + ALIGN(size) +  ALIGN(META_SIZE);

	new_block->size = block->size - (ALIGN(size) +  ALIGN(META_SIZE));
	new_block->status = STATUS_FREE;

	if (block->next == NULL)
		new_block->next = NULL;
	else
		new_block->next = block->next;

	block->next = new_block;
	block->size = ALIGN(size);
	block->status = STATUS_ALLOC;

	return block;
}

struct block_meta *find_free_block(struct block_meta *head, size_t size)
{
	struct block_meta *current_block = head;

	while (current_block != NULL) {
		// coalesce all consecutive free blocks
		while (current_block->next && current_block->status == STATUS_FREE && current_block->next->status == STATUS_FREE) {
			struct block_meta *next_block = current_block->next;

			current_block->next = next_block->next;
			current_block->size += next_block->size;
			current_block->status = STATUS_FREE;
		}

		// the new formed block is larger than the requested size,
		// so we don't have to request space for another block
		if (current_block->status == STATUS_FREE && current_block->size >= ALIGN(size)) {
			current_block->status = STATUS_ALLOC;
			return current_block;
		}
		current_block = current_block->next;
	}

	return NULL;
}

void *os_malloc(size_t size)
{
	struct block_meta *last_block;

	if (heap_prealloc)
		global_base = NULL;

	if (size <= 0) {
		heap_prealloc = 0;
		return NULL;
	}

	// create a list head
	if (!global_base) {
		// first heap prealloc
		last_block = request_space(NULL, size, MMAP_THRESHOLD);
		global_base = last_block;
	} else {
		last_block = find_free_block(global_base, size);
		// did not find a good free block so the size must be allocated
		if (last_block == NULL) {
			last_block = request_space(global_base, size, MMAP_THRESHOLD);
		} else {
			if (last_block->size > ALIGN(size) + ALIGN(META_SIZE))
				last_block = split_block(last_block, size);
		}
	}

	heap_prealloc = 0;
	return (void *)last_block + ALIGN(META_SIZE);
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (heap_prealloc)
		global_base = NULL;

	if (size <= 0 || nmemb <= 0) {
		heap_prealloc = 0;
		return NULL;
	}

	struct block_meta *last_block;

	// create a list head
	if (!global_base) {
		// first heap prealloc
		last_block = request_space(NULL, nmemb * size, sysconf(_SC_PAGESIZE));
		global_base = last_block;
	} else {
		last_block = find_free_block(global_base, nmemb * size);
		// did not find a good free block so the size must be allocated
		if (last_block == NULL) {
			last_block = request_space(global_base, nmemb * size, sysconf(_SC_PAGESIZE));
		} else {
			if (last_block->size > ALIGN(nmemb * size) + ALIGN(META_SIZE))
				last_block = split_block(last_block, nmemb * size);
		}
	}

	heap_prealloc = 0;
	memset((void *)last_block + ALIGN(META_SIZE), 0, size);
	return (void *)last_block + ALIGN(META_SIZE);
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	struct block_meta *block = ptr - ALIGN(META_SIZE);

	// a mapped block must be unmapped for reuse
	if (block->status == STATUS_MAPPED) {
		if (global_base == block) {
			global_base = global_base->next;
		} else {
			struct block_meta *list_head = global_base;

			if (list_head != NULL) {
				while (list_head->next != block)
					list_head = list_head->next;

				list_head->next = block->next;
			}
		}
		int ret = munmap(block, ALIGN(META_SIZE) + ALIGN(block->size));

		DIE(ret == -1, "munmap failed");
	} else if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;
	}
}

size_t min(size_t a, size_t b)
{
	if (a < b)
		return a;
	return b;
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);

	if (size <= 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block = ptr - ALIGN(META_SIZE);

	if (block->size == size)
		return ptr;

	if (block->status == STATUS_FREE)
		return NULL;

	if (block->status == STATUS_MAPPED || ALIGN(size) + ALIGN(META_SIZE) >= MMAP_THRESHOLD) {
		void *new_ptr = os_malloc(size);

		if (!new_ptr)
			return NULL;

		memcpy(new_ptr, ptr, min(block->size, ALIGN(size)));
		os_free(ptr);
		if (heap_prealloc)
			heap_prealloc = 0;
		return new_ptr;
	}

	return ptr;
}

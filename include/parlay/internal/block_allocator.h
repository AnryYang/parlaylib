// A concurrent allocator for any fixed type T
// Keeps a local pool per processor
// Grabs list_size elements from a global pool if empty, and
// Returns list_size elements to the global pool when local pool=2*list_size
// Keeps track of number of allocated elements.
// Probably more efficient than a general purpose allocator

#ifndef PARLAY_BLOCK_ALLOCATOR_H_
#define PARLAY_BLOCK_ALLOCATOR_H_

#include <cstdio>
#include <cstdlib>
#include <cmath>

#include <atomic>

#include "concurrent_stack.h"
#include "memory_size.h"

#include "../utilities.h"

namespace parlay {

struct block_allocator {
private:

  static const size_t default_list_bytes = (1 << 22) - 64; // in bytes
  static const size_t pad_size = 256;

  struct block {
    block* next;
  };

  using block_p = block*;

  struct alignas(64) thread_list {
    size_t sz;
    block_p head;
    block_p mid;
    char cache_line[pad_size];
    thread_list() : sz(0), head(NULL) {};
  };

  bool initialized{false};
  //block_p initialize_list(block_p);
  //block_p get_list();
  concurrent_stack<char*> pool_roots;
  concurrent_stack<block_p> global_stack;
  thread_list* local_lists;

  size_t list_length;
  size_t max_blocks;
  size_t block_size_;
  //std::atomic<size_t> blocks_allocated;
  size_t blocks_allocated;
  //char* allocate_blocks(size_t num_blocks);

public:
  //int block_allocator::thread_count 
  const int thread_count;
  //void* alloc();
  //void free(void*);
  //void reserve(size_t n);
  //void clear();
  //void print_stats();
  size_t block_size () {return block_size_;}
  size_t num_allocated_blocks() {return blocks_allocated;}
  //size_t num_used_blocks();

  //~block_allocator();
  //block_allocator(size_t block_size,
  //                size_t reserved_blocks = 0, 
  //                size_t list_length_ = 0, 
  //		    size_t max_blocks_ = 0);
  block_allocator() : thread_count(num_workers()) {};

  // Allocate a new list of list_length elements

  auto initialize_list(block_p start) -> block_p {
    parallel_for (0, list_length - 1, [&] (size_t i) {
					block_p p =  (block_p) (((char*) start) + i * block_size_);
					p->next = (block_p) (((char*) p) + block_size_);
				      }, 1000, true);
    block_p last = (block_p) (((char*) start) + (list_length-1) * block_size_);
    last->next = NULL;
    return start;
  }

  size_t num_used_blocks() {
    size_t free_blocks = global_stack.size()*list_length;
    for (int i = 0; i < thread_count; ++i) 
      free_blocks += local_lists[i].sz;
    return blocks_allocated - free_blocks;
  }

  auto allocate_blocks(size_t num_blocks) -> char* {
    char* start = (char*) aligned_alloc(pad_size,
					num_blocks * block_size_+ pad_size);
    //char* start = (char*) parlay::my_alloc(num_blocks * block_size_);
    if (start == NULL) {
      fprintf(stderr, "Cannot allocate space in block_allocator");
      exit(1); }

    parlay::fetch_and_add(&blocks_allocated, num_blocks); // atomic

    if (blocks_allocated > max_blocks) {
      fprintf(stderr, "Too many blocks in block_allocator, change max_blocks");
      exit(1);  }

    pool_roots.push(start); // keep track so can free later
    return start;
  }

  // Either grab a list from the global pool, or if there is none
  // then allocate a new list
  auto get_list() -> block_p {
    maybe<block_p> rem = global_stack.pop();
    if (rem) return *rem;
    block_p start = (block_p) allocate_blocks(list_length);
    return initialize_list(start);
  }

  // Allocate n elements across however many lists are needed (rounded up)
  void reserve(size_t n) {
    size_t num_lists = thread_count + ceil(n / (double)list_length);
    char* start = allocate_blocks(list_length*num_lists);
    parallel_for(0, num_lists, [&] (size_t i) {
				 block_p offset = (block_p) (start + i * list_length * block_size_);
				 global_stack.push(initialize_list(offset));
			       });
  }

  void print_stats() {
    size_t used = num_used_blocks();
    size_t allocated = num_allocated_blocks();
    size_t size = block_size();
    std::cout << "Used: " << used << ", allocated: " << allocated
	      << ", block size: " << size
	      << ", bytes: " << size*allocated << std::endl;
  }

  block_allocator(size_t block_size,
		  size_t reserved_blocks = 0, 
		  size_t list_length_ = 0, 
		  size_t max_blocks_ = 0) : thread_count(num_workers()) {
    blocks_allocated = 0;
    block_size_ = block_size;
    if (list_length_ == 0)
      list_length = default_list_bytes / block_size;
    else list_length = list_length_ / block_size;
    if  (max_blocks_ == 0)
      max_blocks = (3*getMemorySize()/block_size)/4;
    else max_blocks = max_blocks_;

    reserve(reserved_blocks);

    // all local lists start out empty
    local_lists = new thread_list[thread_count];
    initialized = true;
  }

  void clear() {
    if (num_used_blocks() > 0) 
      std::cout << "Warning: not clearing memory pool, block_size=" << block_size()
	   << " : allocated blocks remain" << std::endl;
    else {
      // clear lists
      for (int i = 0; i < thread_count; ++i) 
	local_lists[i].sz = 0;
  
      // throw away all allocated memory
      maybe<char*> x;
      while ((x = pool_roots.pop())) std::free(*x);
      pool_roots.clear();
      global_stack.clear();
      blocks_allocated = 0;
    }
  }

  ~block_allocator() {
    clear();
    delete[] local_lists;
  }

  void free(void* ptr) {
    block_p new_node = (block_p) ptr;
    int id = worker_id();

    if (local_lists[id].sz == list_length+1) {
      local_lists[id].mid = local_lists[id].head;
    } else if (local_lists[id].sz == 2*list_length) {
      global_stack.push(local_lists[id].mid->next);
      local_lists[id].mid->next = NULL;
      local_lists[id].sz = list_length;
    }
    new_node->next = local_lists[id].head;
    local_lists[id].head = new_node;
    local_lists[id].sz++;
  }

  inline void* alloc() {
    int id = worker_id();

    if (local_lists[id].sz == 0)  {
      local_lists[id].head = get_list();
      local_lists[id].sz = list_length;
    }

    local_lists[id].sz--;
    block_p p = local_lists[id].head;
    local_lists[id].head = local_lists[id].head->next;

    return (void*) p;
  }

};

}  // namespace parlay

#endif  // PARLAY_BLOCK_ALLOCATOR_H_

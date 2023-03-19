#include <algorithm>
#include <assert.h>
#include <cstring>
#include <execinfo.h>
#include <iostream>
#include <setjmp.h>
#include <stdlib.h>
#include <vector>

//#include "../include/heap.hpp"
#include <heap.hpp>

using std::cout, std::endl, std::vector, std::hex, std::dec;

namespace GC
{

	/**
	 * Initialises the heap singleton and saves the address
	 * of the calling stack frame as the stack_top. Presumeably
	 * this address points to the stack frame of the compiled
	 * LLVM executable after linking.
	 */
	void Heap::init()
	{
		Heap *heap = Heap::the();
		heap->m_stack_top = static_cast<uintptr_t *>(__builtin_frame_address(1));
	}

	/**
	 * Disposes the heap at program exit.
	 */
	void Heap::dispose()
	{
		Heap *heap = Heap::the();
		delete heap;
	}

	/**
	 * Allocates a given amount of bytes on the heap.
	 *
	 * @param size The amount of bytes to be allocated.
	 *
	 * @return  A pointer to the address where the memory
	 *          has been allocated. This pointer is supposed
	 *          to be casted to and object pointer.
	 */
	void *Heap::alloc(size_t size)
	{

		// Singleton
		Heap *heap = Heap::the();

		if (size < 0)
		{
			cout << "Heap: Cannot alloc less than 0B. No bytes allocated." << endl;
			return nullptr;
		}

		if (heap->m_size + size > HEAP_SIZE)
		{
			heap->collect();
			// If collect failed, crash with OOM error
			assert(heap->m_size + size <= HEAP_SIZE && "Heap: Out Of Memory");
		}

		// If a chunk was recycled, return the old chunk address
		uintptr_t *reused_chunk = heap->try_recycle_chunks(size);
		if (reused_chunk != nullptr)
		{
			return static_cast<void *>(reused_chunk);
		}

		// If no free chunks was found (reused_chunk is a nullptr),
		// then create a new chunk
		auto new_chunk = new Chunk;
		new_chunk->size = size;
		new_chunk->start = (uintptr_t *)(heap->m_heap + heap->m_size);

		heap->m_size += size;

		heap->m_allocated_chunks.push_back(new_chunk);

		// new_chunk should probably be a unique pointer, if that isn't implicit already
		return new_chunk->start;
	}

	/**
	 * Tries to recycle used and freed chunks that are
	 * already allocated objects by the OS but freed
	 * from our Heap. This reduces the amount of GC
	 * objects slightly which saves time from malloc'ing
	 * memory from the OS.
	 *
	 * @param size  Amount of bytes needed for the object
	 *              which is about to be allocated.
	 *
	 * @returns If a chunk is found and recycled, a
	 *          pointer to the allocated memory for
	 *          the object is returned. If not, a
	 *          nullptr is returned to signify no
	 *          chunks were found.
	 */
	uintptr_t *Heap::try_recycle_chunks(size_t size)
	{
		auto heap = Heap::the();
		// Check if there are any freed chunks large enough for current request
		for (size_t i = 0; i < heap->m_freed_chunks.size(); i++)
		{
			auto chunk = Heap::get_at(heap->m_freed_chunks, i);
			auto iter = heap->m_freed_chunks.begin();
			advance(iter, i);
			if (chunk->size > size)
			{
				// Split the chunk, use one part and add the remaining part to
				// the list of freed chunks
				size_t diff = chunk->size - size;

				auto chunk_complement = new Chunk;
				chunk_complement->size = diff;
				chunk_complement->start = chunk->start + chunk->size;

				heap->m_freed_chunks.erase(iter);
				heap->m_freed_chunks.push_back(chunk_complement);
				heap->m_allocated_chunks.push_back(chunk);

				return chunk->start;
			}
			else if (chunk->size == size)
			{
				// Reuse the whole chunk
				heap->m_freed_chunks.erase(iter);
				heap->m_allocated_chunks.push_back(chunk);
				return chunk->start;
			}
		}
		return nullptr;
	}

	/**
	 * Collection phase of the garbage collector. When
	 * an allocation is requested and there is no space
	 * left on the heap, a collection is triggered. This
	 * function is private so that the user cannot trigger
	 * a collection unneccessarily.
	 */
	void Heap::collect()
	{
		// Get instance
		auto heap = Heap::the();

		// get current stack
		auto stack_bottom = reinterpret_cast<uintptr_t *>(__builtin_frame_address(0));

		uintptr_t *stack_top = heap->m_stack_top != nullptr ? heap->m_stack_top : (uintptr_t *)0;

		auto work_list = heap->m_allocated_chunks;
		mark(stack_bottom, stack_top, work_list);

		sweep(heap);

		free(heap);
	}

	/**
	 * Iterates through the stack, if an element on the stack points to a chunk,
	 * called a root chunk, that chunk is marked (i.e. reachable).
	 * Then it recursively follows all chunks which are possibly reachable from
	 * the root chunk and mark those chunks.
	 * If a chunk is marked it is removed from the worklist, since it's no longer of
	 * concern for this method.
	 *
	 * @param start    Pointer to the start of the stack frame.
	 * @param end      Pointer to the end of the stack frame.
	 * @param worklist The currently allocated chunks, which haven't been marked.
	 */
	void Heap::mark(uintptr_t *start, const uintptr_t *end, vector<Chunk *> &worklist)
	{
		cout << "--- mark() was called ---\n" << endl;
		if (Heap::get_profiler_mode())
			Profiler::record(MarkStart);
		// To find adresses thats in the worklist
		for (; start <= end; start++)
		{
			auto it = worklist.begin();
			auto stop = worklist.end();
			// for (auto it = worklist.begin(); it != worklist.end();) {
			while (it != stop) // while (it != stop)
			{
				Chunk *chunk = *it;

				auto c_start = reinterpret_cast<uintptr_t>(chunk->start);
				auto c_size = reinterpret_cast<uintptr_t>(chunk->size);
				auto c_end = reinterpret_cast<uintptr_t>(c_start + c_size);

/* 				cout << "Value of Start:\t\t" << start << endl;
				cout << "Start points to:\t" << hex << *start << endl;
				cout << "Chunk start:\t\t" << hex << c_start << endl;
				cout << "Chunk end:\t\t" << hex << c_end << "\n" << endl; */

				// Check if the stack pointer aligns with the chunk
				if (c_start <= *start && *start < c_end)
				{

					if (!chunk->marked)
					{
						if (Heap::get_profiler_mode())
							Profiler::record(ChunkMarked, chunk);
						chunk->marked = true;
						cout << "Marked this chunk ^\n" << endl;
						// Remove the marked chunk from the worklist
						it = worklist.erase(it);
						// Recursively call mark, to see if the reachable chunk further points to another chunk
						mark((uintptr_t *)c_start, (uintptr_t *)c_end, worklist);

						//mark_step(c_start, c_end, worklist);
					}
					else
					{
						++it;
					}
				}
				else
				{
					++it;
				}
			}
		}
	}

	// Testing a strategy where if a pointer on the stack is pointing to a chunk, nested chunks,
	// that are not located on the stack frame, will possibly be adjecent to the found chunk, 
	// allowing for a different, more efficient strategy, that doesn't have to scan the stack frame
	void Heap::mark_step(uintptr_t start, uintptr_t end, vector<Chunk *> &worklist) {

		// Should loop through the chunk size, such that if the object holds a pointer,
		// wherever that pointer is located in the object, that pointer, to another chunk,
		// gets detected

		cout << "--- mark_step() was called ---\n" << endl;
		for (; start <= end; start += sizeof(uintptr_t)) { 

			auto it = worklist.begin();
			auto end = worklist.end();
			
			while (it != end) {

				Chunk *chunk = *it;
				auto c_start = reinterpret_cast<uintptr_t>(chunk->start);
				auto c_size = reinterpret_cast<uintptr_t>(chunk->size);
				auto c_end = reinterpret_cast<uintptr_t>(c_start + c_size);

				cout << "Value of Start:\t\t" << start << endl;
				cout << "Chunk start:\t\t" << hex << c_start << endl;
				cout << "Chunk end:\t\t" << hex << c_end << "\n" << endl;

				if (c_start <= start && start < c_end) {

					if (!chunk->marked) {
						// Mark the chunk and erase it from the worklist
						chunk->marked = true;
						it = worklist.erase(it);
						cout << "Marked this chunk ^\n" << endl;
						// Update the memory location we want to look at
						//memory_location = c_end;
						mark_step(c_start, c_end, worklist);
					}
					else {
						it++;
					}
				}
				else {
					it++;
				}
			}
		}
	}

	/**
	 * Sweeps the heap, unmarks the marked chunks for the next cycle,
	 * adds the unmarked nodes to the list of freed chunks; to be freed.
	 *
	 * @param heap Pointer to the heap singleton instance.
	 */
	void Heap::sweep(Heap *heap)
	{
		cout << "--- sweep() was called ---" << endl;
		auto iter = heap->m_allocated_chunks.begin();
		auto stop = heap->m_allocated_chunks.end();
		// This cannot "iter != stop", results in seg fault, since the end gets updated, I think.
		while (iter != heap->m_allocated_chunks.end())
		{
			Chunk *chunk = *iter;

			// Unmark the marked chunks for the next iteration.
			if (chunk->marked)
			{
				chunk->marked = false;
				++iter;
			}
			else
			{
				// Add the unmarked chunks to freed chunks and remove from
				// the list of allocated chunks
				Profiler::record(ChunkSwept, chunk);
				heap->m_freed_chunks.push_back(chunk);
				iter = heap->m_allocated_chunks.erase(iter);
			}
		}
	}

	/**
	 * Frees chunks that was moved to the list m_freed_chunks
	 * by the sweep phase. If there are more than a certain
	 * amount of free chunks, delete the free chunks to
	 * avoid cluttering.
	 *
	 * @param heap  Heap singleton instance, only for avoiding
	 *              redundant calls to the singleton get
	 */
	void Heap::free(Heap *heap)
	{
		cout << "--- free() was called ---" << endl;
		if (heap->m_freed_chunks.size() > FREE_THRESH)
		{
			while (heap->m_freed_chunks.size())
			{
				auto chunk = heap->m_freed_chunks.back();
				heap->m_freed_chunks.pop_back();
				cout << "Freed chunk was deleted" << endl;
				delete chunk;
			}
		}
		// if there are chunks but not more than FREE_THRESH
		else if (heap->m_freed_chunks.size())
		{
			// essentially, always check for overlap between
			// chunks before finishing the allocation
			free_overlap(heap);
		}
	}

	/**
	 * Checks for overlaps between freed chunks of memory
	 * and removes overlapping chunks while prioritizing
	 * the chunks at lower addresses.
	 *
	 * @param heap  Heap singleton instance, only for avoiding
	 *              redundant calls to the singleton get
	 *
	 * @note Maybe this should be changed to prioritizing
	 *       larger chunks.
	 */
	void Heap::free_overlap(Heap *heap)
	{
		std::vector<Chunk *> filtered;
		size_t i = 0;
		// filtered.push_back(heap->m_freed_chunks.at(i++));
		filtered.push_back(Heap::get_at(heap->m_freed_chunks, i++));
		cout << filtered.back()->start << endl;
		for (; i < heap->m_freed_chunks.size(); i++)
		{
			auto prev = filtered.back();
			// auto next = heap->m_freed_chunks.at(i);
			auto next = Heap::get_at(heap->m_freed_chunks, i);
			auto p_start = (uintptr_t)(prev->start);
			auto p_size = (uintptr_t)(prev->size);
			auto n_start = (uintptr_t)(next->start);
			if (n_start >= (p_start + p_size))
			{
				filtered.push_back(next);
			}
		}
		heap->m_freed_chunks.swap(filtered);
	}

	// ----- ONLY DEBUGGING -----------------------------------------------------------------------

	/**
	 * Prints the result of Heap::init() and a dummy value
	 * for the current stack frame for reference.
	 */
	void Heap::check_init()
	{
		auto heap = Heap::the();
		cout << "Heap addr:\t" << heap << "\n";
		cout << "GC m_stack_top:\t" << heap->m_stack_top << "\n";
		auto stack_bottom = reinterpret_cast<uintptr_t *>(__builtin_frame_address(0));
		cout << "GC stack_bottom:\t" << stack_bottom << endl;
	}

	/**
	 * Conditional collection, only to be used in debugging
	 *
	 * @param flags Bitmap of flags
	 */
	void Heap::collect(CollectOption flags)
	{
		set_profiler(true);

		if (Heap::get_profiler_mode())
			Profiler::record(CollectStart);

		cout << "DEBUG COLLECT\nFLAGS: ";
		if (flags & MARK)
			cout << "\n - MARK";
		if (flags & SWEEP)
			cout << "\n - SWEEP";
		if (flags & FREE)
			cout << "\n - FREE";
		cout << "\n";

		auto heap = Heap::the();

		// get the frame adress, whwere local variables and saved registers are located
		auto stack_bottom = reinterpret_cast<uintptr_t *>(__builtin_frame_address(0));
		cout << "Stack bottom in collect:\t" << stack_bottom << "\n";
		uintptr_t *stack_top = heap->m_stack_top;

		cout << "Stack end in collect:\t " << stack_top << endl;
		auto work_list = heap->m_allocated_chunks;

		if (flags & MARK)
			mark(stack_bottom, stack_top, work_list);

		if (flags & SWEEP)
			sweep(heap);

		if (flags & FREE)
			free(heap);
	}

	// Mark child references from the root references
	void mark_test(vector<Chunk *> &worklist)
	{
		while (worklist.size() > 0)
		{
			Chunk *ref = worklist.back();
			worklist.pop_back();
			Chunk *child = (Chunk *)ref; // this is probably not correct
			if (child != nullptr && !child->marked)
			{
				child->marked = true;
				worklist.push_back(child);
				mark_test(worklist);
			}
		}
	}

	// Mark the root references and look for child references to them
	void mark_from_roots(uintptr_t *start, const uintptr_t *end)
	{
		vector<Chunk *> worklist;
		for (; start > end; start--)
		{
			if (*start % 8 == 0)
			{ // all pointers must be aligned as double words
				Chunk *ref = (Chunk *)*start;
				if (ref != nullptr && !ref->marked)
				{
					ref->marked = true;
					worklist.push_back(ref);
					mark_test(worklist);
				}
			}
		}
	}

	// For testing purposes
	void Heap::print_line(Chunk *chunk)
	{
		cout << "Marked: " << chunk->marked << "\nStart adr: " << chunk->start << "\nSize: " << chunk->size << " B\n"
			 << endl;
	}

	void Heap::print_worklist(std::vector<Chunk *> &list)
	{
		for (auto cp : list)
			cout << "Chunk at:\t" << cp->start << "\nSize:\t\t" << cp->size << "\n";
		cout << endl;
	}

	void Heap::print_contents()
	{
		auto heap = Heap::the();
		if (heap->m_allocated_chunks.size())
		{
			cout << "\nALLOCATED CHUNKS #" << dec << heap->m_allocated_chunks.size() << endl;
			for (auto chunk : heap->m_allocated_chunks)
				print_line(chunk);
		}
		else
		{
			cout << "NO ALLOCATIONS\n" << endl;
		}
		if (heap->m_freed_chunks.size())
		{
			cout << "\nFREED CHUNKS #" << dec << heap->m_freed_chunks.size() << endl;
			for (auto fchunk : heap->m_freed_chunks)
				print_line(fchunk);
		}
		else
		{
			cout << "NO FREED CHUNKS" << endl;
		}
	}

	void Heap::set_profiler(bool mode)
	{
		auto heap = Heap::the();
		heap->m_profiler_enable = mode;
	}

	void Heap::print_allocated_chunks(Heap *heap) {
		cout << "--- Allocated Chunks ---\n" << endl;
		for (auto chunk : heap->m_allocated_chunks) {
			print_line(chunk);
		}
	}
}
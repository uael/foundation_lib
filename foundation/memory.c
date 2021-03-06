/* memory.c  -  Foundation library  -  Public Domain  -  2013 Mattias Jansson / Rampant Pixels
 *
 * This library provides a cross-platform foundation library in C11 providing basic support
 * data types and functions to write applications and games in a platform-independent fashion.
 * The latest source code is always available at
 *
 * https://github.com/rampantpixels/foundation_lib
 *
 * This library is put in the public domain; you can redistribute it and/or modify it without
 * any restrictions.
 */

#include <foundation/foundation.h>
#include <foundation/internal.h>

#if FOUNDATION_PLATFORM_WINDOWS
#  include <foundation/windows.h>
#endif
#if FOUNDATION_PLATFORM_POSIX
#  include <foundation/posix.h>
#  include <sys/mman.h>
#endif

#if FOUNDATION_PLATFORM_LINUX || FOUNDATION_PLATFORM_ANDROID
#  include <malloc.h>
#endif

#if FOUNDATION_PLATFORM_PNACL
#  include <stdlib.h>
#endif

#if FOUNDATION_PLATFORM_WINDOWS && (FOUNDATION_COMPILER_GCC || FOUNDATION_COMPILER_CLANG)
#  include <malloc.h>
#endif

/*lint -e728 */
static const memory_tracker_t _memory_no_tracker;
static memory_system_t _memory_system;
static bool _memory_initialized;

typedef FOUNDATION_ALIGN(8) struct {
	void*               storage;
	void*               end;
	atomicptr_t         head;
	size_t              size;
	size_t              maxchunk;
} atomic_linear_memory_t;

typedef FOUNDATION_ALIGN(8) struct {
	atomic64_t allocations_total;
	atomic64_t allocations_current;
	atomic64_t allocated_total;
	atomic64_t allocated_current;
} memory_statistics_atomic_t;

FOUNDATION_STATIC_ASSERT(sizeof(memory_statistics_t) == sizeof(memory_statistics_atomic_t),
                         "statistics sizes differs");

static atomic_linear_memory_t _memory_temporary;
static memory_statistics_atomic_t _memory_stats;

#if BUILD_ENABLE_MEMORY_GUARD
#define MEMORY_GUARD_VALUE 0xDEADBEEF
#endif

#if BUILD_ENABLE_MEMORY_TRACKER

static memory_tracker_t _memory_tracker;
static memory_tracker_t _memory_tracker_preinit;

static void
_memory_track(void* addr, size_t size);

static void
_memory_untrack(void* addr);

#else

#define _memory_track(addr, size ) do { (void)sizeof( (addr) ); (void)sizeof( (size) ); } while(0)
#define _memory_untrack(addr) do { (void)sizeof( (addr) ); } while(0)

#endif

// Max align must at least be sizeof( size_t )
#if FOUNDATION_PLATFORM_ANDROID
#  define FOUNDATION_MAX_ALIGN  8
#else
#  define FOUNDATION_MAX_ALIGN  16
#endif

static void
_atomic_allocate_initialize() {
	size_t storagesize = foundation_config().temporary_memory;
	if (!storagesize) {
		memset(&_memory_temporary, 0, sizeof(_memory_temporary));
		return;
	}
	_memory_temporary.storage   = memory_allocate(0, storagesize, 16, MEMORY_PERSISTENT);
	_memory_temporary.end       = pointer_offset(_memory_temporary.storage, storagesize);
	_memory_temporary.size      = storagesize;
	_memory_temporary.maxchunk  = (storagesize / 8);
	//We must avoid using storage address or tracking will incorrectly match temporary allocation
	//with the full temporary memory storage
	atomic_storeptr(&_memory_temporary.head, pointer_offset(_memory_temporary.storage, 8));
}

static void
_atomic_allocate_finalize(void) {
	void* storage = _memory_temporary.storage;
	memset(&_memory_temporary, 0, sizeof(_memory_temporary));
	if (storage)
		memory_deallocate(storage);
}

static void*
_atomic_allocate_linear(size_t chunksize) {
	void* old_head;
	void* new_head;
	void* return_pointer = 0;

	do {
		old_head = atomic_loadptr(&_memory_temporary.head);
		new_head = pointer_offset(old_head, chunksize);

		return_pointer = old_head;

		if (new_head > _memory_temporary.end) {
			//Avoid using raw storage pointer for tracking, see _atomic_allocate_initialize
			return_pointer = pointer_offset(_memory_temporary.storage, 8);
			new_head = pointer_offset(return_pointer, chunksize);
		}
	}
	while (!atomic_cas_ptr(&_memory_temporary.head, new_head, old_head));

	return return_pointer;
}

static FOUNDATION_CONSTCALL FOUNDATION_FORCEINLINE int unsigned
_memory_get_align(unsigned int align) {
	//All alignment in memory code is built around higher alignments
	//being multiples of lower alignments (powers of two).
	//4, 8, 16, ...
#if FOUNDATION_PLATFORM_ANDROID
	return align > 0 ? FOUNDATION_MAX_ALIGN : 0;
#elif FOUNDATION_PLATFORM_WINDOWS
	if (align < FOUNDATION_SIZE_POINTER)
		return FOUNDATION_SIZE_POINTER;
	align = math_align_poweroftwo(align);
	return (align < FOUNDATION_MAX_ALIGN) ? align : FOUNDATION_MAX_ALIGN;
#else
	if (align < FOUNDATION_SIZE_POINTER)
		return align ? FOUNDATION_SIZE_POINTER : 0;
	align = math_align_poweroftwo(align);
	return (align < FOUNDATION_MAX_ALIGN) ? align : FOUNDATION_MAX_ALIGN;
#endif
}

static FOUNDATION_CONSTCALL FOUNDATION_FORCEINLINE int unsigned
_memory_get_align_forced(unsigned int align) {
	align = _memory_get_align(align);
	return align > FOUNDATION_SIZE_POINTER ? align : FOUNDATION_SIZE_POINTER;
}

static FOUNDATION_CONSTCALL void*
_memory_align_pointer(void* p, unsigned int align) {
	uintptr_t address;
	uintptr_t mask;
	if (!p || !align)
		return p;

	address = (uintptr_t)p;
	mask = (uintptr_t)align - 1; //Align is always power-of-two
	if (address & mask) {
		address = (address & ~mask) + align;
		p = (void*)address;
	}

	return p;
}

int
_memory_initialize(const memory_system_t memory) {
	int ret;
	_memory_system = memory;
	memset(&_memory_stats, 0, sizeof(_memory_stats));
	ret = _memory_system.initialize();
	if (ret == 0) {
		_memory_initialized = true;
		_atomic_allocate_initialize();
#if BUILD_ENABLE_MEMORY_TRACKER
		if (_memory_tracker_preinit.initialize)
			memory_set_tracker(_memory_tracker_preinit);
#endif
	}
	return ret;
}

void
_memory_finalize(void) {
#if BUILD_ENABLE_MEMORY_TRACKER
	_memory_tracker_preinit = _memory_tracker;
	if (_memory_tracker.finalize)
		_memory_tracker.finalize();
#endif
	_atomic_allocate_finalize();
	if (_memory_system.thread_finalize)
		_memory_system.thread_finalize();
	memory_set_tracker(_memory_no_tracker);
	_memory_system.finalize();
	_memory_initialized = false;
}

#if BUILD_ENABLE_MEMORY_GUARD

static void*
_memory_guard_initialize(void* memory, size_t size) {
	int guard_loop;
	uint32_t* guard_header = pointer_offset(memory, FOUNDATION_MAX_ALIGN);
	uint32_t* guard_footer = pointer_offset(memory, size + FOUNDATION_MAX_ALIGN * 2);
	*(size_t*)memory = size;
	for (guard_loop = 0; guard_loop < FOUNDATION_MAX_ALIGN / 4; ++guard_loop) {
		*guard_header++ = MEMORY_GUARD_VALUE;
		*guard_footer++ = MEMORY_GUARD_VALUE;
	}
	return pointer_offset(memory, FOUNDATION_MAX_ALIGN * 2);
}

static void*
_memory_guard_verify(void* memory) {
	int guard_loop;
	size_t    size = *(size_t*)pointer_offset(memory, -FOUNDATION_MAX_ALIGN * 2);
	uint32_t* guard_header = pointer_offset(memory, -FOUNDATION_MAX_ALIGN);
	uint32_t* guard_footer = pointer_offset(memory, size);
	for (guard_loop = 0; guard_loop < FOUNDATION_MAX_ALIGN / 4; ++guard_loop) {
		if (*guard_header != MEMORY_GUARD_VALUE)
			FOUNDATION_ASSERT_MSG(*guard_header == MEMORY_GUARD_VALUE, "Memory underwrite");
		if (*guard_footer != MEMORY_GUARD_VALUE)
			FOUNDATION_ASSERT_MSG(*guard_footer == MEMORY_GUARD_VALUE, "Memory overwrite");
		guard_header++;
		guard_footer++;
	}
	return pointer_offset(memory, -FOUNDATION_MAX_ALIGN * 2);
}

#endif

void*
memory_allocate(hash_t context, size_t size, unsigned int align, unsigned int hint) {
	void* p = 0;
	if (_memory_temporary.storage && (hint & MEMORY_TEMPORARY)) {
		unsigned int tmpalign = _memory_get_align_forced(align);
		if (size + tmpalign < _memory_temporary.maxchunk) {
			p = _memory_align_pointer(_atomic_allocate_linear(size + tmpalign), tmpalign);
			FOUNDATION_ASSERT(!((uintptr_t)p & 1));
			if (hint & MEMORY_ZERO_INITIALIZED)
				memset(p, 0, (size_t)size);
		}
	}
	if (!p)
		p = _memory_system.allocate(context ? context : memory_context(), size, align, hint);
	_memory_track(p, size);
	return p;
}

void*
memory_reallocate(void* p, size_t size, unsigned int align, size_t oldsize) {
	FOUNDATION_ASSERT_MSG((p < _memory_temporary.storage) ||
	                      (p >= _memory_temporary.end), "Trying to reallocate temporary memory");
	_memory_untrack(p);
	p = _memory_system.reallocate(p, size, align, oldsize);
	_memory_track(p, size);
	return p;
}

void
memory_deallocate(void* p) {
	if ((p < _memory_temporary.storage) || (p >= _memory_temporary.end))
		_memory_system.deallocate(p);
	_memory_untrack(p);
}

memory_statistics_t
memory_statistics(void) {
	memory_statistics_t stats;
	memcpy(&stats, &_memory_stats, sizeof(memory_statistics_t));
	return stats;
}

#if BUILD_ENABLE_MEMORY_CONTEXT

FOUNDATION_DECLARE_THREAD_LOCAL(memory_context_t*, memory_context, 0)

void
memory_context_push(hash_t context_id) {
	memory_context_t* context = get_thread_memory_context();
	if (!context) {
		context = memory_allocate(0, sizeof(memory_context_t) +
		                          (sizeof(hash_t) * foundation_config().memory_context_depth),
		                          0, MEMORY_PERSISTENT | MEMORY_ZERO_INITIALIZED);
		set_thread_memory_context(context);
	}
	context->context[ context->depth ] = context_id;
	if (context->depth < (foundation_config().memory_context_depth - 1))
		++context->depth;
}

void
memory_context_pop(void) {
	memory_context_t* context = get_thread_memory_context();
	if (context && (context->depth > 0))
		--context->depth;
}

hash_t
memory_context(void) {
	memory_context_t* context = get_thread_memory_context();
	return (context && (context->depth > 0)) ? context->context[ context->depth - 1 ] : 0;
}

void
memory_context_thread_finalize(void) {
	memory_context_t* context = get_thread_memory_context();
	if (context)
		memory_deallocate(context);
	set_thread_memory_context(0);
}

#else 

#undef memory_context_push
#undef memory_context_pop
#undef memory_context
#undef memory_context_thread_finalize

void
memory_context_push(hash_t context_id) {
	FOUNDATION_UNUSED(context_id);
}

void
memory_context_pop(void) {
}

hash_t
memory_context(void) {
	return 0;
}

void
memory_context_thread_finalize(void) {
}

#endif

void
memory_thread_finalize(void) {
	if (_memory_system.thread_finalize)
		_memory_system.thread_finalize();
}

#if FOUNDATION_PLATFORM_WINDOWS && (FOUNDATION_SIZE_POINTER != 4)

typedef long (*NtAllocateVirtualMemoryFn)(HANDLE, void**, ULONG, size_t*, ULONG, ULONG);
static NtAllocateVirtualMemoryFn NtAllocateVirtualMemory = 0;

#endif

static void*
_memory_allocate_malloc_raw(size_t size, unsigned int align, unsigned int hint) {
	FOUNDATION_UNUSED(hint);

	//If we align manually, we must be able to retrieve the original pointer for passing to free()
	//Thus all allocations need to go through that path

#if FOUNDATION_PLATFORM_WINDOWS

#  if FOUNDATION_SIZE_POINTER == 4
#    if BUILD_ENABLE_MEMORY_GUARD
	char* memory = _aligned_malloc((size_t)size + FOUNDATION_MAX_ALIGN * 3, align);
	if (memory)
		memory = _memory_guard_initialize(memory, (size_t)size);
	return memory;
#    else
	return _aligned_malloc((size_t)size, align);
#    endif
#  else
	unsigned int padding, extra_padding = 0;
	size_t allocate_size;
	char* raw_memory;
	void* memory;
	long vmres;

	if (!(hint & MEMORY_32BIT_ADDRESS)) {
		padding = (align > FOUNDATION_SIZE_POINTER ? align : FOUNDATION_SIZE_POINTER);
#if BUILD_ENABLE_MEMORY_GUARD
		extra_padding = FOUNDATION_MAX_ALIGN * 3;
#endif
		raw_memory = _aligned_malloc((size_t)size + padding + extra_padding, align);
		if (raw_memory) {
			memory = raw_memory +
			         padding; //Will be aligned since padding is multiple of alignment (minimum align/pad is pointer size)
			*((void**)memory - 1) = raw_memory;
			FOUNDATION_ASSERT(!((uintptr_t)raw_memory & 1));
			FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
#if BUILD_ENABLE_MEMORY_GUARD
			memory = _memory_guard_initialize(memory, size);
			FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
#endif
			return memory;
		}
		log_errorf(HASH_MEMORY, ERROR_OUT_OF_MEMORY,
		           STRING_CONST("Unable to allocate %" PRIsize " bytes of memory"), size);
		return 0;
	}

#    if BUILD_ENABLE_MEMORY_GUARD
	extra_padding = FOUNDATION_MAX_ALIGN * 3;
#    endif

	allocate_size = size + FOUNDATION_SIZE_POINTER + extra_padding + align;
	raw_memory = 0;

	vmres = NtAllocateVirtualMemory(INVALID_HANDLE_VALUE, (void**)&raw_memory, 1, &allocate_size,
	                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (vmres != 0) {
		log_errorf(HASH_MEMORY, ERROR_OUT_OF_MEMORY,
		           STRING_CONST("Unable to allocate %" PRIsize " bytes of memory in low 32bit address space"), size);
		return 0;
	}

	memory = _memory_align_pointer(raw_memory + FOUNDATION_SIZE_POINTER, align);
	*((void**)memory - 1) = (void*)((uintptr_t)raw_memory | 1);
#    if BUILD_ENABLE_MEMORY_GUARD
	memory = _memory_guard_initialize(memory, size);
#    endif
	FOUNDATION_ASSERT(!((uintptr_t)raw_memory & 1));
	FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
	return memory;
#  endif

#else

#  if FOUNDATION_SIZE_POINTER > 4
	if (!(hint & MEMORY_32BIT_ADDRESS))
#  endif
	{
#if BUILD_ENABLE_MEMORY_GUARD
		size_t extra_padding = FOUNDATION_MAX_ALIGN * 3;
#else
		size_t extra_padding = 0;
#endif
		size_t allocate_size = size + align + FOUNDATION_SIZE_POINTER + extra_padding;
		char* raw_memory = malloc(allocate_size);
		if (raw_memory) {
			void* memory = _memory_align_pointer(raw_memory + FOUNDATION_SIZE_POINTER, align);
			*((void**)memory - 1) = raw_memory;
			FOUNDATION_ASSERT(!((uintptr_t)raw_memory & 1));
			FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
#if BUILD_ENABLE_MEMORY_GUARD
			memory = _memory_guard_initialize(memory, size);
			FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
#endif
			return memory;
		}
		log_errorf(HASH_MEMORY, ERROR_OUT_OF_MEMORY,
		           STRING_CONST("Unable to allocate %" PRIsize " bytes of memory (%" PRIsize " requested)"), size,
		           allocate_size);
		return 0;
	}

#  if FOUNDATION_SIZE_POINTER > 4

	size_t allocate_size;
	char* raw_memory;
	void* memory;

#    if BUILD_ENABLE_MEMORY_GUARD
	unsigned int extra_padding = FOUNDATION_MAX_ALIGN * 3;
#else
	unsigned int extra_padding = 0;
#    endif

	allocate_size = size + align + FOUNDATION_SIZE_POINTER * 2 + extra_padding;

#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#    ifndef MAP_32BIT
	//On MacOSX app needs to be linked with -pagezero_size 10000 -image_base 100000000 to
	// 1) Free up low 4Gb address range by reducing page zero size
	// 2) Move executable base address above 4Gb to free up more memory address space
#define MMAP_REGION_START ((uintptr_t)0x10000)
#define MMAP_REGION_END   ((uintptr_t)0x80000000)
	static atomicptr_t baseaddr = { (void*)MMAP_REGION_START };
	bool retried = false;
	do {
		raw_memory = mmap(atomic_loadptr(&baseaddr), allocate_size, PROT_READ | PROT_WRITE,
		                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);
		if (((uintptr_t)raw_memory >= MMAP_REGION_START) &&
		        (uintptr_t)(raw_memory + allocate_size) < MMAP_REGION_END) {
			atomic_storeptr(&baseaddr, pointer_offset(raw_memory, allocate_size));
			break;
		}
		if (raw_memory && (raw_memory != MAP_FAILED)) {
			if (munmap(raw_memory, allocate_size) < 0)
				log_warn(HASH_MEMORY, WARNING_SYSTEM_CALL_FAIL,
				         STRING_CONST("Failed to munmap pages outside 32-bit range"));
		}
		raw_memory = 0;
		if (retried)
			break;
		retried = true;
		atomic_storeptr(&baseaddr, (void*)MMAP_REGION_START);
	}
	while (true);
#    else
	raw_memory = mmap(0, allocate_size, PROT_READ | PROT_WRITE,
	                  MAP_32BIT | MAP_PRIVATE | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);
	if (raw_memory == MAP_FAILED) {
		raw_memory = mmap(0, allocate_size, PROT_READ | PROT_WRITE,
		                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);
		if (raw_memory == MAP_FAILED)
			raw_memory = 0;
		if ((uintptr_t)raw_memory > 0xFFFFFFFFULL) {
			if (munmap(raw_memory, allocate_size) < 0)
				log_warn(HASH_MEMORY, WARNING_SYSTEM_CALL_FAIL,
				         STRING_CONST("Failed to munmap pages outside 32-bit range"));
			raw_memory = 0;
		}
	}
#    endif
	if (!raw_memory) {
		string_const_t errmsg = system_error_message(0);
		log_errorf(HASH_MEMORY, ERROR_OUT_OF_MEMORY,
		           STRING_CONST("Unable to allocate %" PRIsize " bytes of memory in low 32bit address space: %.*s"),
		           size, STRING_FORMAT(errmsg));
		return 0;
	}

	memory = _memory_align_pointer(raw_memory + FOUNDATION_SIZE_POINTER * 2, align);
	*((uintptr_t*)memory - 1) = ((uintptr_t)raw_memory | 1);
	*((uintptr_t*)memory - 2) = (uintptr_t)allocate_size;
	FOUNDATION_ASSERT(!((uintptr_t)raw_memory & 1));
	FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
#    if BUILD_ENABLE_MEMORY_GUARD
	memory = _memory_guard_initialize(memory, size);
	FOUNDATION_ASSERT(!((uintptr_t)memory & 1));
#    endif

	return memory;

#  endif

#endif
}

static void*
_memory_allocate_malloc(hash_t context, size_t size, unsigned  int align, unsigned int hint) {
	void* block;
	FOUNDATION_UNUSED(context);
	align = _memory_get_align(align);
	block = _memory_allocate_malloc_raw(size, align, hint);
	if (block && (hint & MEMORY_ZERO_INITIALIZED))
		memset(block, 0, (size_t)size);
	return block;
}

static void
_memory_deallocate_malloc(void* p) {
#if FOUNDATION_SIZE_POINTER == 4
	if (!p)
		return;
#  if BUILD_ENABLE_MEMORY_GUARD
	p = _memory_guard_verify(p);
#  endif
#  if FOUNDATION_PLATFORM_WINDOWS
	_aligned_free(p);
#  else
	free(*((void**)p - 1));
#  endif

#else

	uintptr_t raw_ptr;

	if (!p)
		return;

#  if BUILD_ENABLE_MEMORY_GUARD
	p = _memory_guard_verify(p);
#  endif
	raw_ptr = *((uintptr_t*)p - 1);
	if (raw_ptr & 1) {
		raw_ptr &= ~(uintptr_t)1;
#  if FOUNDATION_PLATFORM_WINDOWS
		if (VirtualFree((void*)raw_ptr, 0, MEM_RELEASE) == 0)
			log_warnf(HASH_MEMORY, WARNING_SYSTEM_CALL_FAIL,
			          STRING_CONST("Failed to VirtualFree 0x%" PRIfixPTR),
			          (uintptr_t)raw_ptr);
#  else
		uintptr_t raw_size = *((uintptr_t*)p - 2);
		if (munmap((void*)raw_ptr, raw_size) < 0)
			log_warnf(HASH_MEMORY, WARNING_SYSTEM_CALL_FAIL,
			          STRING_CONST("Failed to munmap 0x%" PRIfixPTR " size %" PRIsize),
			          (uintptr_t)raw_ptr, raw_size);
#  endif
	}
	else {
#  if FOUNDATION_PLATFORM_WINDOWS
		_aligned_free((void*)raw_ptr);
#  else
		free((void*)raw_ptr);
#  endif
	}

#endif
}

static void*
_memory_reallocate_malloc(void* p, size_t size, unsigned  int align, size_t oldsize) {
#if ( FOUNDATION_SIZE_POINTER == 4 ) && FOUNDATION_PLATFORM_WINDOWS
	FOUNDATION_UNUSED(oldsize);
	align = _memory_get_align(align);
#  if BUILD_ENABLE_MEMORY_GUARD
	if (p) {
		p = _memory_guard_verify(p);
		p = _aligned_realloc(p, (size_t)size + FOUNDATION_MAX_ALIGN * 3, align);
	}
	else {
		p = _aligned_malloc((size_t)size + FOUNDATION_MAX_ALIGN * 3, align);
	}
	if (p)
		p = _memory_guard_initialize(p, (size_t)size);
	return p;
#  else
	return _aligned_realloc(p, (size_t)size, align);
#  endif
#else
	void* memory;
	void* raw_p;

	align = _memory_get_align(align);

	memory = p;
#  if BUILD_ENABLE_MEMORY_GUARD
	if (memory)
		memory = _memory_guard_verify(memory);
#  endif
	raw_p = memory ? *((void**)memory - 1) : nullptr;
	memory = nullptr;

#if FOUNDATION_PLATFORM_WINDOWS
	if (raw_p && !((uintptr_t)raw_p & 1)) {
		size_t padding = (align > FOUNDATION_SIZE_POINTER ? align : FOUNDATION_SIZE_POINTER);
#  if BUILD_ENABLE_MEMORY_GUARD
		size_t extra_padding = FOUNDATION_MAX_ALIGN * 3;
#  else
		size_t extra_padding = 0;
#  endif
		void* raw_memory = _aligned_realloc(raw_p, size + padding + extra_padding, align ? align : 8);
		if (raw_memory) {
			memory = pointer_offset(raw_memory, padding);
			*((void**)memory - 1) = raw_memory;
#  if BUILD_ENABLE_MEMORY_GUARD
			memory = _memory_guard_initialize(memory, size);
#  endif
		}
	}
	else {
#  if FOUNDATION_SIZE_POINTER == 4
		memory = _memory_allocate_malloc_raw(size, align, 0U);
#  else
		memory = _memory_allocate_malloc_raw(size, align,
		                                     (raw_p && ((uintptr_t)raw_p < 0xFFFFFFFFULL)) ?
		                                     MEMORY_32BIT_ADDRESS : 0U);
#  endif
		if (p && memory && oldsize)
			memcpy(memory, p, (size < oldsize) ? size : oldsize);
		_memory_deallocate_malloc(p);
	}

#else //!FOUNDATION_PLATFORM_WINDOWS

//If we're on ARM the realloc can return a 16-bit aligned address, causing raw pointer store to SIGILL
//Realigning does not work since the realloc memory copy preserve cannot be done properly. Revert to normal alloc-and-copy
//Same with alignment, since we cannot guarantee that the returned memory block offset from start of actual memory block
//is the same in the reallocated block as the original block, we need to alloc-and-copy to get alignment
//Memory guard introduces implicit alignments as well so alloc-and-copy for that
#if !FOUNDATION_ARCH_ARM && !FOUNDATION_ARCH_ARM_64 && !BUILD_ENABLE_MEMORY_GUARD
	if (!align && raw_p && !((uintptr_t)raw_p & 1)) {
		void* raw_memory = realloc(raw_p, (size_t)size + FOUNDATION_SIZE_POINTER);
		if (raw_memory) {
			*(void**)raw_memory = raw_memory;
			memory = pointer_offset(raw_memory, FOUNDATION_SIZE_POINTER);
		}
	}
	else
#endif
	{
#  if FOUNDATION_SIZE_POINTER == 4
#    if !BUILD_ENABLE_LOG
		FOUNDATION_UNUSED(raw_p);
#    endif
		memory = _memory_allocate_malloc_raw(size, align, 0U);
#  else
		memory = _memory_allocate_malloc_raw(size, align,
		                                     (raw_p && ((uintptr_t)raw_p < 0xFFFFFFFFULL)) ?
		                                     MEMORY_32BIT_ADDRESS : 0U);
#  endif
		if (p && memory && oldsize)
			memcpy(memory, p, (size < oldsize) ? (size_t)size : (size_t)oldsize);
		_memory_deallocate_malloc(p);
	}

#endif

	if (!memory) {
		string_const_t errmsg = system_error_message(0);
		log_panicf(HASH_MEMORY, ERROR_OUT_OF_MEMORY,
		           STRING_CONST("Unable to reallocate memory (%" PRIsize " -> %" PRIsize " @ 0x%" PRIfixPTR ", raw 0x%"
		                        PRIfixPTR "): %.*s"),
		           oldsize, size, (uintptr_t)p, (uintptr_t)raw_p, STRING_FORMAT(errmsg));
	}

	return memory;
#endif
}

static int
_memory_initialize_malloc(void) {
#if FOUNDATION_PLATFORM_WINDOWS && ( FOUNDATION_SIZE_POINTER > 4 )
	NtAllocateVirtualMemory = (NtAllocateVirtualMemoryFn)GetProcAddress(GetModuleHandleA("ntdll.dll"),
	                          "NtAllocateVirtualMemory");
#endif
	return 0;
}

static void
_memory_finalize_malloc(void) {
}

memory_system_t
memory_system_malloc(void) {
	memory_system_t memsystem;
	memsystem.allocate = _memory_allocate_malloc;
	memsystem.reallocate = _memory_reallocate_malloc;
	memsystem.deallocate = _memory_deallocate_malloc;
	memsystem.initialize = _memory_initialize_malloc;
	memsystem.finalize = _memory_finalize_malloc;
	memsystem.thread_finalize = 0;
	return memsystem;
}

#if BUILD_ENABLE_MEMORY_TRACKER

void
memory_set_tracker(memory_tracker_t tracker) {
	memory_tracker_t old_tracker = _memory_tracker;

	if ((old_tracker.track == tracker.track) && (old_tracker.untrack == tracker.untrack))
		return;

	_memory_tracker = _memory_no_tracker;

	if (old_tracker.abort)
		old_tracker.abort();

	if (old_tracker.finalize)
		old_tracker.finalize();

	if (_memory_initialized) {
		if (tracker.initialize)
			tracker.initialize();

		_memory_tracker = tracker;
	}
	else {
		_memory_tracker_preinit = tracker;
	}
}

static void
_memory_track(void* addr, size_t size) {
	if (_memory_tracker.track)
		_memory_tracker.track(addr, size);
}

static void
_memory_untrack(void* addr) {
	if (_memory_tracker.untrack)
		_memory_tracker.untrack(addr);
}

struct memory_tag_t {
	atomicptr_t   address;
	size_t        size;
	void*         trace[14];
};

typedef FOUNDATION_ALIGN(8) struct memory_tag_t memory_tag_t;

static memory_tag_t* _memory_tags;
static atomic32_t    _memory_tag_next;
static bool          _memory_tracker_initialized;

static int
_memory_tracker_initialize(void) {
	if (!_memory_tracker_initialized) {
		size_t size = sizeof(memory_tag_t) * foundation_config().memory_tracker_max;
		_memory_tags = memory_allocate(0, size, 16, MEMORY_PERSISTENT | MEMORY_ZERO_INITIALIZED);

#if BUILD_ENABLE_MEMORY_STATISTICS
		atomic_incr64(&_memory_stats.allocations_total);
		atomic_incr64(&_memory_stats.allocations_current);
		atomic_add64(&_memory_stats.allocated_total, (int64_t)size);
		atomic_add64(&_memory_stats.allocated_current, (int64_t)size);
#endif
		_memory_tracker_initialized = true;
	}

	return 0;
}

static void
_memory_tracker_cleanup(void) {
	_memory_tracker_initialized = false;
	if (_memory_tags) {
		memory_deallocate(_memory_tags);

#if BUILD_ENABLE_MEMORY_STATISTICS
		size_t size = sizeof(memory_tag_t) * foundation_config().memory_tracker_max;
		atomic_decr64(&_memory_stats.allocations_current);
		atomic_add64(&_memory_stats.allocated_current, -(int64_t)size);
#endif

		_memory_tags = nullptr;
	}
}

static void
_memory_tracker_finalize(void) {
	_memory_tracker_initialized = false;
	if (_memory_tags) {
		unsigned int it;

		for (it = 0; it < foundation_config().memory_tracker_max; ++it) {
			memory_tag_t* tag = _memory_tags + it;
			if (atomic_loadptr(&tag->address)) {
				char tracebuf[512];
				string_t trace = stacktrace_resolve(tracebuf, sizeof(tracebuf), tag->trace,
				                                    sizeof(tag->trace)/sizeof(tag->trace[0]), 0);
				void* addr = atomic_loadptr(&tag->address);
				log_warnf(HASH_MEMORY, WARNING_MEMORY,
				          STRING_CONST("Memory leak: %" PRIsize " bytes @ 0x%" PRIfixPTR " : tag %d\n%.*s"),
				          tag->size, (uintptr_t)addr, it, (int)trace.length, trace.str);
			}
		}
	}
	_memory_tracker_cleanup();
}

static void
_memory_tracker_track(void* addr, size_t size) {
	if (addr && _memory_tracker_initialized) {
		size_t limit = foundation_config().memory_tracker_max * 2;
		size_t loop = 0;
		do {
			int32_t tag = atomic_exchange_and_add32(&_memory_tag_next, 1);
			while (tag >= (int32_t)foundation_config().memory_tracker_max) {
				int32_t newtag = tag % (int32_t)foundation_config().memory_tracker_max;
				if (atomic_cas32(&_memory_tag_next, newtag + 1, tag + 1))
					tag = newtag;
				else
					tag = atomic_exchange_and_add32(&_memory_tag_next, 1);
			}
			if (atomic_cas_ptr(&_memory_tags[tag].address, addr, 0)) {
				_memory_tags[tag].size = size;
				stacktrace_capture(_memory_tags[tag].trace,
				                   sizeof(_memory_tags[tag].trace)/sizeof(_memory_tags[tag].trace[0]), 3);
				break;
			}
		}
		while (++loop < limit);

		//if (loop >= limit)
		//	log_warnf(HASH_MEMORY, WARNING_SUSPICIOUS, STRING_CONST("Unable to track allocation: 0x%" PRIfixPTR), (uintptr_t)addr);

#if BUILD_ENABLE_MEMORY_STATISTICS
		atomic_incr64(&_memory_stats.allocations_total);
		atomic_incr64(&_memory_stats.allocations_current);
		atomic_add64(&_memory_stats.allocated_total, (int64_t)size);
		atomic_add64(&_memory_stats.allocated_current, (int64_t)size);
#endif
	}
}

static void
_memory_tracker_untrack(void* addr) {
	int32_t tag = 0;
	size_t size = 0;
	if (addr && _memory_tracker_initialized) {
		int32_t maxtag = (int32_t)foundation_config().memory_tracker_max;
		int32_t iend = atomic_load32(&_memory_tag_next) % maxtag;
		int32_t itag = iend ? iend - 1 : maxtag - 1;
		while (true) {
			void* tagaddr = atomic_loadptr(&_memory_tags[itag].address);
			if (addr == tagaddr) {
				tag = itag + 1;
				size = _memory_tags[itag].size;
				break;
			}
			if (itag == iend)
				break;
			else if (itag)
				--itag;
			else
				itag = (int32_t)foundation_config().memory_tracker_max - 1;
		}
	}
	if (tag && size) {
		--tag;
		atomic_storeptr(&_memory_tags[tag].address, 0);
#if BUILD_ENABLE_MEMORY_STATISTICS
		atomic_decr64(&_memory_stats.allocations_current);
		atomic_add64(&_memory_stats.allocated_current, -(int64_t)size);
#endif
	}
	//else if (addr && _memory_tracker_initialized) {
	//	log_warnf(HASH_MEMORY, WARNING_SUSPICIOUS, STRING_CONST("Untracked deallocation: 0x%" PRIfixPTR), (uintptr_t)addr);
	//}
}

#endif

memory_tracker_t
memory_tracker_local(void) {
	memory_tracker_t tracker = _memory_no_tracker;
#if BUILD_ENABLE_MEMORY_TRACKER
	tracker.track = _memory_tracker_track;
	tracker.untrack = _memory_tracker_untrack;
	tracker.initialize = _memory_tracker_initialize;
	tracker.abort = _memory_tracker_cleanup;
	tracker.finalize = _memory_tracker_finalize;
#endif
	return tracker;
}

memory_tracker_t
memory_tracker_none(void) {
	return _memory_no_tracker;
}

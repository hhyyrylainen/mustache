#include "memory_manager.hpp"
#include <mustache/utils/logger.hpp>
#include <mustache/utils/profiler.hpp>
#include <mustache/utils/container_map.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#ifdef __APPLE__
#include <sys/malloc.h>
#else
#include <malloc.h>
#endif


#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#endif

// Only x86/x86_64 have CPUID in the way this code expects.
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
  #define MUSTACHE_HAS_X86_CPUID 1
#else
  #define MUSTACHE_HAS_X86_CPUID 0
#endif

#if MUSTACHE_HAS_X86_CPUID
#ifdef _MSC_VER
#include <intrin.h>
  void cpuid(int info[4], int leaf, int subleaf) {
      __cpuidex(info, leaf, subleaf);
  }
#else
#include <cpuid.h>
void cpuid(int info[4], int leaf, int subleaf) {
    __cpuid_count(leaf, subleaf,
                  reinterpret_cast<unsigned int&>(info[0]),
                  reinterpret_cast<unsigned int&>(info[1]),
                  reinterpret_cast<unsigned int&>(info[2]),
                  reinterpret_cast<unsigned int&>(info[3]));
}
#endif
#endif


namespace {

    std::size_t get_l1d_cache_size() {
#if MUSTACHE_HAS_X86_CPUID
        int info[4];
        std::size_t cache_size = 0;

        // Leaf 4, subleaf = 0,1,2... перебираем все кэши
        for (int i = 0;; ++i) {
            cpuid(info, 4, i);
            int cache_type = info[0] & 0x1F;
            if (cache_type == 0)  // 0 = no more caches
                break;

            int level      = (info[0] >> 5) & 0x7;
            bool isData    = cache_type == 1;
            if (level == 1 && isData) {
                int line_size     = (info[1] & 0xFFF) + 1;
                int partitions    = ((info[1] >> 12) & 0x3FF) + 1;
                int ways          = ((info[1] >> 22) & 0x3FF) + 1;
                int sets          = info[2] + 1;
                cache_size = std::size_t(line_size) * static_cast<size_t>(partitions * ways * sets);
                break;
            }
        }
        return cache_size;  // в байтах
#else
        // Not available. Seems safe as the l1d cache size variable seems unused
        return 0;
#endif
    }

    std::mutex pages_mutex;
    std::unordered_map<void*, size_t> pages;
}

using namespace mustache;

const size_t MemoryManager::cache_size_l1d = get_l1d_cache_size();

void* MemoryManager::allocate(size_t size, size_t align) noexcept {
    MUSTACHE_PROFILER_BLOCK_LVL_3("MemoryManager::allocate");
#ifdef _WIN32
#define ALIGNED_ALLOC(size, align) _aligned_malloc(size, align)
#elif defined(ANDROID)
#define ALIGNED_ALLOC(size, align) memalign(align, size)
#else
#define ALIGNED_ALLOC(size, align) aligned_alloc(align, size)
#endif
    constexpr size_t min_align = 8;
    if (align < min_align) {
        align = min_align;
        size = (size + align - 1) & ~(align - 1);
    }

    // This will crash loudly if align is not a power of two.
    // We could assert here, but that would make debug builds much slower

#ifdef __APPLE__
    void* ptr = (align < 8) ? malloc(size) : ALIGNED_ALLOC(size, align);
#else
    void* ptr = (align == 0) ? malloc(size) : ALIGNED_ALLOC(size, align);
#endif

#undef ALIGNED_ALLOC

    // Can't fall back on Windows as cannot mix malloc and aligned free!
#ifndef _WIN32
    if (ptr == nullptr) {
        ptr = malloc(size);
    }
#endif
    return ptr;
}

void* MemoryManager::allocateAndClear(size_t size, size_t align) noexcept {
    MUSTACHE_PROFILER_BLOCK_LVL_3("MemoryManager::allocateAndClear");
    void* result = allocate(size, align);
    memset(result, 0, size);
    return result;
}

void MemoryManager::deallocate(void* ptr) noexcept {
    MUSTACHE_PROFILER_BLOCK_LVL_3("MemoryManager::deallocate");
    if (ptr) {
#ifdef _WIN32
        _aligned_free(ptr);
#else
        free(ptr);
#endif
    }
}

void* MemoryManager::allocateSmart(size_t size, size_t align, bool allow_pages, bool allow_large_pages) noexcept {
    void* ptr = nullptr;
    if (allow_large_pages && size >= large_page_size) {
        std::lock_guard lock {pages_mutex};
#ifdef _WIN32
        ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
#else
        ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) ptr = nullptr;
#endif
        if (ptr) {
            pages[ptr] = size;
            return ptr;
        }
    }

    if (allow_pages) {
        std::lock_guard lock {pages_mutex};
#ifdef _WIN32
        ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) ptr = nullptr;
#endif
        if (ptr) {
            pages[ptr] = size;
            return ptr;
        }
    }
    return allocate(size, align);
}

void MemoryManager::deallocateSmart(void* ptr) noexcept {
    if (reinterpret_cast<uintptr_t>(ptr) % page_size == 0) {
        const auto find_res = pages.find(ptr);
        if (find_res != pages.end()) {
            std::lock_guard lock {pages_mutex};
#ifdef _WIN32
            VirtualFree(ptr, 0, MEM_RELEASE);
#else
            munmap(ptr, find_res->second);
#endif
            pages.erase(find_res);
            return;
        }
    }

    deallocate(ptr);
}


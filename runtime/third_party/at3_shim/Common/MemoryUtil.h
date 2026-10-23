#pragma once
// Stand-in for PPSSPP's aligned allocation helpers.
#include <cstddef>
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
#endif

inline void *AllocateAlignedMemory(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    void *ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size == 0 ? 1 : size) != 0) return nullptr;
    return ptr;
#endif
}
inline void FreeAlignedMemory(void *ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

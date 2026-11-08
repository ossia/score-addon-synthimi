#pragma once

// Counts the allocations made while `g_watch` is set: every replaceable
// operator new and new[] (plain, aligned, nothrow), and on glibc malloc,
// calloc and realloc too, which a library may call directly. Included by
// one file of a test program: it replaces them for the whole program.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
std::atomic<bool> g_watch{false};
std::atomic<int> g_allocs{0};

inline void countAllocation() noexcept
{
  if(g_watch.load(std::memory_order_relaxed))
    g_allocs.fetch_add(1, std::memory_order_relaxed);
}
}

#if defined(__GLIBC__)
// glibc's own entry points: malloc and the others below forward to them,
// operator new calls them directly, so that each allocation counts once
extern "C" void* __libc_malloc(std::size_t);
extern "C" void* __libc_calloc(std::size_t, std::size_t);
extern "C" void* __libc_realloc(void*, std::size_t);
extern "C" void* __libc_memalign(std::size_t, std::size_t);
extern "C" void __libc_free(void*);

extern "C" void* malloc(std::size_t n)
{
  countAllocation();
  return __libc_malloc(n);
}
extern "C" void* calloc(std::size_t n, std::size_t size)
{
  countAllocation();
  return __libc_calloc(n, size);
}
extern "C" void* realloc(void* p, std::size_t n)
{
  countAllocation();
  return __libc_realloc(p, n);
}
extern "C" void free(void* p)
{
  __libc_free(p);
}
#endif

namespace
{
inline void* counted(std::size_t n) noexcept
{
  countAllocation();
  n = n ? n : 1;
#if defined(__GLIBC__)
  return __libc_malloc(n);
#else
  return std::malloc(n);
#endif
}

inline void* counted(std::size_t n, std::align_val_t a) noexcept
{
  countAllocation();
  const auto align = static_cast<std::size_t>(a);
  n = n ? n : 1;
#if defined(__GLIBC__)
  return __libc_memalign(align, n);
#else
  // aligned_alloc wants a multiple of the alignment
  return std::aligned_alloc(align, (n + align - 1) / align * align);
#endif
}
}

void* operator new(std::size_t n)
{
  if(void* p = counted(n))
    return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n)
{
  if(void* p = counted(n))
    return p;
  throw std::bad_alloc{};
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
  return counted(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
  return counted(n);
}
void* operator new(std::size_t n, std::align_val_t a)
{
  if(void* p = counted(n, a))
    return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a)
{
  if(void* p = counted(n, a))
    return p;
  throw std::bad_alloc{};
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
  return counted(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
  return counted(n, a);
}

void operator delete(void* p) noexcept
{
  std::free(p);
}
void operator delete[](void* p) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
  std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
  std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(p);
}

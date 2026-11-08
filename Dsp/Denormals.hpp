#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <cstdint>

// The SSE control register; Emscripten emulates SSE on WebAssembly SIMD,
// which has none
#if (defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)) && !defined(__EMSCRIPTEN__)
#include <xmmintrin.h>
#define SYNTHIMI_HAS_MXCSR 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define SYNTHIMI_HAS_FPCR 1
#endif

namespace synthimi
{
/*! Denormals flushed to zero while it lives, where the FPU does it: x86's
 *  MXCSR (FTZ and DAZ) and, with `Arm`, AArch64's FPCR (FZ). Elsewhere
 *  kHardware is false, and a synth flushes its filters' states itself.
 */
template <bool Arm = true>
struct ScopedFlushDenormals
{
#if SYNTHIMI_HAS_MXCSR
  static constexpr bool kHardware = true;
  unsigned int saved = _mm_getcsr();
  ScopedFlushDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
  ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#elif SYNTHIMI_HAS_FPCR
  static constexpr bool kHardware = Arm;
  std::uint64_t saved = Arm ? fpcr() : 0;
  ScopedFlushDenormals() noexcept
  {
    if constexpr(Arm)
      setFpcr(saved | (std::uint64_t(1) << 24));
  }
  ~ScopedFlushDenormals()
  {
    if constexpr(Arm)
      setFpcr(saved);
  }
  static std::uint64_t fpcr() noexcept
  {
    std::uint64_t r;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(r));
    return r;
  }
  static void setFpcr(std::uint64_t r) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(r)); }
#else
  static constexpr bool kHardware = false;
  ScopedFlushDenormals() noexcept = default;
#endif
  ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
  ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
};
}

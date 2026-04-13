#pragma once
#include <cstdint>

namespace hft {

inline std::uint64_t rdtsc_start() {
  unsigned lo, hi;
#if defined(__x86_64__) || defined(__i386__)
  asm volatile(
      "CPUID\n\t"
      "RDTSC\n\t"
      : "=a"(lo), "=d"(hi)
      :
      : "%rbx", "%rcx");
  return (static_cast<std::uint64_t>(hi) << 32U) | lo;
#else
  return 0;
#endif
}

inline std::uint64_t rdtsc_end() {
  unsigned lo, hi;
#if defined(__x86_64__) || defined(__i386__)
  asm volatile(
      "RDTSCP\n\t"
      "mov %%edx, %0\n\t"
      "mov %%eax, %1\n\t"
      "CPUID\n\t"
      : "=r"(hi), "=r"(lo)
      :
      : "%rax", "%rbx", "%rcx", "%rdx");
  return (static_cast<std::uint64_t>(hi) << 32U) | lo;
#else
  return 0;
#endif
}

}  // namespace hft


#pragma once
#include <cstdint>

inline uint64_t rdtsc_start() {
    unsigned lo, hi;
    asm volatile (
        "CPUID\n\t"
        "RDTSC\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "%rbx", "%rcx");
    return ((uint64_t)hi << 32) | lo;
}

inline uint64_t rdtsc_end() {
    unsigned lo, hi;
    asm volatile (
        "RDTSCP\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        "CPUID\n\t"
        : "=r"(hi), "=r"(lo)
        :
        : "%rax", "%rbx", "%rcx", "%rdx");
    return ((uint64_t)hi << 32) | lo;
}

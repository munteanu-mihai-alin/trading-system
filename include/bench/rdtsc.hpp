
#pragma once
#include <cstdint>

inline uint64_t rdtsc_start(){
    unsigned lo, hi;
    asm volatile ("CPUID\n\tRDTSC\n\t":"=a"(lo),"=d"(hi)::"%rbx","%rcx");
    return ((uint64_t)hi<<32)|lo;
}

inline uint64_t rdtsc_end(){
    unsigned lo, hi;
    asm volatile ("RDTSCP\n\tmov %%edx,%0\n\tmov %%eax,%1\n\tCPUID\n\t":"=r"(hi),"=r"(lo)::"%rax","%rbx","%rcx","%rdx");
    return ((uint64_t)hi<<32)|lo;
}

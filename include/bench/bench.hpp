
#pragma once
#include <vector>
#include <algorithm>
#include <iostream>

inline void report(std::vector<uint64_t>& v){
    std::sort(v.begin(), v.end());

    auto pct = [&](double p){
        return v[(size_t)(p * v.size())];
    };

    double avg = 0;
    for(auto x : v) avg += x;
    avg /= v.size();

    std::cout << "Latency (cycles): "
              << "p50=" << pct(0.50)
              << " p99=" << pct(0.99)
              << " p999=" << pct(0.999)
              << " max=" << v.back()
              << " avg=" << avg
              << std::endl;
}

#pragma once
#include <algorithm>
#include <cstdint>
#include <ostream>
#include <vector>

namespace hft {

struct LatencySummary {
    double p50 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double max = 0.0;
    double avg = 0.0;
};

inline LatencySummary summarize_cycles(std::vector<std::uint64_t> samples) {
    LatencySummary out{};
    if (samples.empty()) return out;

    std::sort(samples.begin(), samples.end());

    auto at = [&](double p) -> double {
        std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]);
    };

    double sum = 0.0;
    for (auto x : samples) sum += static_cast<double>(x);

    out.p50 = at(0.50);
    out.p99 = at(0.99);
    out.p999 = at(0.999);
    out.max = static_cast<double>(samples.back());
    out.avg = sum / static_cast<double>(samples.size());
    return out;
}

inline std::ostream& operator<<(std::ostream& os, const LatencySummary& s) {
    os << "Latency (cycles): p50=" << s.p50 << " p99=" << s.p99 << " p999=" << s.p999
       << " max=" << s.max << " avg=" << s.avg;
    return os;
}

}  // namespace hft

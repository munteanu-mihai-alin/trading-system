#pragma once
#include <vector>

class LatencyModel {
public:
    void record(double ms) { samples.push_back(ms); }

    double mean_latency() const {
        if(samples.empty()) return 1.0;
        double s=0;
        for(double v: samples) s+=v;
        return s / samples.size();
    }

private:
    std::vector<double> samples;
};

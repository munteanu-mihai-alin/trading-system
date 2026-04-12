#pragma once
#include <vector>

namespace hft {

class IVolatilityModel {
public:
    virtual ~IVolatilityModel() = default;
    virtual double annualizedVol(const std::vector<double>& returns) const = 0;
};

}  // namespace hft

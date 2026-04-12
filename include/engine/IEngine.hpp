#pragma once

namespace hft {

class IEngine {
   public:
    virtual ~IEngine() = default;
    virtual void step(int t) = 0;
};

}  // namespace hft

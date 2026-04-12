#pragma once
#include <algorithm>

namespace hft {

class ConnectionSupervisor {
    int attempt_ = 0;
    int max_attempts_ = 10;
    int base_backoff_ms_ = 250;
    int max_backoff_ms_ = 8000;

   public:
    [[nodiscard]] bool should_retry() const { return attempt_ < max_attempts_; }

    [[nodiscard]] int next_backoff_ms() {
        const int backoff =
            std::min(max_backoff_ms_, base_backoff_ms_ * (1 << std::min(attempt_, 5)));
        ++attempt_;
        return backoff;
    }

    void reset() { attempt_ = 0; }
};

}  // namespace hft

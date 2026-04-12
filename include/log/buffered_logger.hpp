#pragma once
#include <fstream>
#include <string>
#include <vector>

namespace hft {

class BufferedLogger {
    std::vector<std::string> buffer_;

   public:
    void log(const std::string& msg) {
        buffer_.push_back(msg);
        if (buffer_.size() >= 1024) flush();
    }

    void flush() {
        std::ofstream out("trading.log", std::ios::app);
        for (const auto& s : buffer_) out << s << '\n';
        buffer_.clear();
    }

    ~BufferedLogger() { flush(); }
};

}  // namespace hft

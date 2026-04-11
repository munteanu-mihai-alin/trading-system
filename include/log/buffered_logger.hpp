#pragma once
#include <vector>
#include <string>
#include <fstream>

class BufferedLogger {
public:
    void log(const std::string& msg) {
        buffer.emplace_back(msg);
        if (buffer.size() >= 1024) flush();
    }

    void flush() {
        std::ofstream out("trading.log", std::ios::app);
        for (auto& s : buffer)
            out << s << '\n';
        buffer.clear();
    }

    ~BufferedLogger() { flush(); }

private:
    std::vector<std::string> buffer;
};

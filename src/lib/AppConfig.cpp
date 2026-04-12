#include "config/AppConfig.hpp"

#include <fstream>
#include <sstream>

namespace hft {

static std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

AppConfig AppConfig::load_from_file(const std::string& path) {
    AppConfig cfg{};
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        const auto key = trim(line.substr(0, pos));
        const auto val = trim(line.substr(pos + 1));

        if (key == "mode") {
            if (val == "live")
                cfg.mode = BrokerMode::Live;
            else if (val == "sim")
                cfg.mode = BrokerMode::Sim;
            else
                cfg.mode = BrokerMode::Paper;
        } else if (key == "host") {
            cfg.host = val;
        } else if (key == "paper_port") {
            cfg.paper_port = std::stoi(val);
        } else if (key == "live_port") {
            cfg.live_port = std::stoi(val);
        } else if (key == "client_id") {
            cfg.client_id = std::stoi(val);
        } else if (key == "top_k") {
            cfg.top_k = std::stoi(val);
        } else if (key == "steps") {
            cfg.steps = std::stoi(val);
        }
    }
    return cfg;
}

}  // namespace hft

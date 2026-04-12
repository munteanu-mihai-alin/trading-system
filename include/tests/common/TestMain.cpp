#include "common/TestFramework.hpp"

int main() {
    int failed = 0;
    for (const auto& [name, fn] : hft::test::Registry::instance().tests) {
        try {
            fn();
            std::cout << "[PASS] " << name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << name << " :: " << e.what() << "\n";
        }
    }
    return failed == 0 ? 0 : 1;
}

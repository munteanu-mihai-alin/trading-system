#pragma once
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hft::test {

struct Registry {
  using Fn = void (*)();
  std::vector<std::pair<std::string, Fn>> tests;

  static Registry& instance() {
    static Registry r;
    return r;
  }

  void add(const std::string& name, Fn fn) { tests.emplace_back(name, fn); }
};

struct Registrar {
  Registrar(const std::string& name, Registry::Fn fn) {
    Registry::instance().add(name, fn);
  }
};

inline void require(bool cond, const std::string& msg) {
  if (!cond)
    throw std::runtime_error(msg);
}

inline void require_close(double a, double b, double eps,
                          const std::string& msg) {
  if (std::fabs(a - b) > eps) {
    throw std::runtime_error(msg + " got=" + std::to_string(a) +
                             " expected=" + std::to_string(b));
  }
}

}  // namespace hft::test

#define HFT_TEST(name)                                  \
  void name();                                          \
  static hft::test::Registrar reg_##name(#name, &name); \
  void name()

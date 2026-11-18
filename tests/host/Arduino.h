// Minimal Arduino stub so the pure-logic headers in shared/ build on a PC.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using std::isnan;

class String {
 public:
  String(const char* s = "") : s_(s ? s : "") {}
  void trim() {
    const auto b = s_.find_first_not_of(" \t\r\n");
    const auto e = s_.find_last_not_of(" \t\r\n");
    s_ = (b == std::string::npos) ? "" : s_.substr(b, e - b + 1);
  }
  void toUpperCase() {
    std::transform(s_.begin(), s_.end(), s_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  }
  bool operator==(const char* o) const { return s_ == o; }

 private:
  std::string s_;
};

// Serial output is dropped (grow_light.h's disabled stubs print).
struct HostSerial {
  template <typename T>
  void print(const T&) {}
  template <typename T>
  void println(const T&) {}
  void println() {}
};
static HostSerial Serial;

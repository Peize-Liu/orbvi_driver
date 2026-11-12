#pragma once

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

namespace orbvi_test {

inline int& failures() {
  static int value = 0;
  return value;
}

inline void check(bool condition, const std::string& expression,
                  const char* file, int line) {
  if (!condition) {
    ++failures();
    std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
  }
}

template <typename T>
void print_value(std::ostream& output, const T& value) {
  if constexpr (std::is_enum<T>::value) {
    output << static_cast<long long>(value);
  } else {
    output << value;
  }
}

template <typename A, typename B>
void check_equal(const A& actual, const B& expected, const std::string& expression,
                 const char* file, int line) {
  if (!(actual == expected)) {
    ++failures();
    std::cerr << file << ':' << line << ": CHECK_EQ failed: " << expression
              << " actual=";
    print_value(std::cerr, actual);
    std::cerr << " expected=";
    print_value(std::cerr, expected);
    std::cerr << '\n';
  }
}

inline int finish() {
  if (failures() != 0) {
    std::cerr << failures() << " unit checks failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

}  // namespace orbvi_test

#define CHECK_TRUE(expression) \
  ::orbvi_test::check((expression), #expression, __FILE__, __LINE__)
#define CHECK_FALSE(expression) \
  ::orbvi_test::check(!(expression), "!(" #expression ")", __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
  ::orbvi_test::check_equal((actual), (expected), #actual " == " #expected, \
                            __FILE__, __LINE__)

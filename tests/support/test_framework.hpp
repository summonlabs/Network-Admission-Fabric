// Network Admission Fabric - minimal self-contained test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// No external dependency and no timeouts: a test runs to completion or it is a
// defect to diagnose.
#ifndef NAF_TEST_FRAMEWORK_HPP
#define NAF_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace naftest {

using TestFn = void (*)();

struct TestCase {
  const char* suite = "";
  const char* name = "";
  TestFn fn = nullptr;
};

class Registry {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(const char* suite, const char* name, TestFn fn) { tests_.push_back(TestCase{suite, name, fn}); }

  [[nodiscard]] const std::vector<TestCase>& tests() const { return tests_; }

  void fail(const char* file, int line, const std::string& message) {
    ++failures_;
    if (failures_ <= 40) {
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, message.c_str());
    } else if (failures_ == 41) {
      std::fprintf(stderr, "  ... further failures suppressed\n");
    }
    current_failed_ = true;
  }

  [[nodiscard]] std::uint64_t failures() const { return failures_; }
  [[nodiscard]] bool current_failed() const { return current_failed_; }
  void begin_test() { current_failed_ = false; }
  [[nodiscard]] std::uint64_t checks() const { return checks_; }
  void count_check() { ++checks_; }

 private:
  std::vector<TestCase> tests_{};
  std::uint64_t failures_ = 0;
  std::uint64_t checks_ = 0;
  bool current_failed_ = false;
};

struct Registrar {
  Registrar(const char* suite, const char* name, TestFn fn) { Registry::instance().add(suite, name, fn); }
};

[[nodiscard]] int run_all(int argc, char** argv);

}  // namespace naftest

#define NAF_TEST(suite_name, test_name)                                                     \
  static void suite_name##_##test_name##_body();                                            \
  static const ::naftest::Registrar naf_registrar_##suite_name##_##test_name(               \
      #suite_name, #test_name, &suite_name##_##test_name##_body);                           \
  static void suite_name##_##test_name##_body()

#define NAF_CHECK(expr)                                                                     \
  do {                                                                                      \
    ::naftest::Registry::instance().count_check();                                          \
    if (!(expr)) {                                                                          \
      ::naftest::Registry::instance().fail(__FILE__, __LINE__, "expected: " #expr);         \
    }                                                                                       \
  } while (false)

#define NAF_REQUIRE(expr)                                                                   \
  do {                                                                                      \
    ::naftest::Registry::instance().count_check();                                          \
    if (!(expr)) {                                                                          \
      ::naftest::Registry::instance().fail(__FILE__, __LINE__, "required: " #expr);         \
      return;                                                                               \
    }                                                                                       \
  } while (false)

#define NAF_REQUIRE_OK(call)                                                                \
  do {                                                                                      \
    const auto naf_status = (call);                                                         \
    ::naftest::Registry::instance().count_check();                                          \
    if (!naf_status.is_ok()) {                                                              \
      ::naftest::Registry::instance().fail(__FILE__, __LINE__,                              \
                                           std::string(#call " failed: ") +                 \
                                               naf_status.to_string());                     \
      return;                                                                               \
    }                                                                                       \
  } while (false)

#define NAF_CHECK_EQ(actual, expected)                                                      \
  do {                                                                                      \
    ::naftest::Registry::instance().count_check();                                          \
    const auto naf_actual = (actual);                                                       \
    const auto naf_expected = (expected);                                                   \
    if (!(naf_actual == naf_expected)) {                                                    \
      ::naftest::Registry::instance().fail(__FILE__, __LINE__,                              \
                                           std::string(#actual " == " #expected " (got ") + \
                                               ::naftest::describe(naf_actual) + " vs " +   \
                                               ::naftest::describe(naf_expected) + ")");    \
    }                                                                                       \
  } while (false)

#define NAF_CHECK_U64(actual, expected)                                                     \
  do {                                                                                      \
    ::naftest::Registry::instance().count_check();                                          \
    const std::uint64_t naf_actual = static_cast<std::uint64_t>(actual);                    \
    const std::uint64_t naf_expected = static_cast<std::uint64_t>(expected);                \
    if (naf_actual != naf_expected) {                                                       \
      ::naftest::Registry::instance().fail(                                                 \
          __FILE__, __LINE__,                                                               \
          std::string(#actual " has value ") + std::to_string(naf_actual) + ", expected " + \
              std::to_string(naf_expected));                                                \
    }                                                                                       \
  } while (false)

namespace naftest {

template <class T>
std::string describe(const T& value) {
  if constexpr (requires { value.value(); }) {
    return std::to_string(static_cast<unsigned long long>(value.value()));
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return "<value>";
  }
}

}  // namespace naftest

#endif  // NAF_TEST_FRAMEWORK_HPP

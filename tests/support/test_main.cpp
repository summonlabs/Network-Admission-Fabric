// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace naftest {

int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
    } else if (std::strcmp(argv[i], "--list") == 0) {
      list_only = true;
    }
  }

  const std::vector<TestCase>& tests = Registry::instance().tests();
  if (list_only) {
    for (const auto& test : tests) {
      std::printf("%s.%s\n", test.suite, test.name);
    }
    return 0;
  }

  std::uint64_t ran = 0;
  std::uint64_t failed = 0;
  for (const auto& test : tests) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (filter != nullptr && full.find(filter) == std::string::npos) continue;
    Registry::instance().begin_test();
    ++ran;
    const std::uint64_t before = Registry::instance().failures();
    std::printf("[ RUN  ] %s\n", full.c_str());
    std::fflush(stdout);
    try {
      test.fn();
    } catch (const std::exception& error) {
      Registry::instance().fail(__FILE__, __LINE__,
                                std::string("uncaught exception: ") + error.what());
    } catch (...) {
      Registry::instance().fail(__FILE__, __LINE__, "uncaught non-standard exception");
    }
    const std::uint64_t after = Registry::instance().failures();
    if (after != before) {
      ++failed;
      std::printf("[ FAIL ] %s\n", full.c_str());
    } else {
      std::printf("[  OK  ] %s\n", full.c_str());
    }
    std::fflush(stdout);
  }

  std::printf("\n%llu test(s) run, %llu failed, %llu check(s), %llu check failure(s)\n",
              static_cast<unsigned long long>(ran), static_cast<unsigned long long>(failed),
              static_cast<unsigned long long>(Registry::instance().checks()),
              static_cast<unsigned long long>(Registry::instance().failures()));
  return failed == 0 ? 0 : 1;
}

}  // namespace naftest

int main(int argc, char** argv) { return naftest::run_all(argc, argv); }

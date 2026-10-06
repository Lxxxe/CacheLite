#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cachelite::test {

class Failure final : public std::runtime_error {
public:
    explicit Failure(std::string message)
        : std::runtime_error(std::move(message)) {
    }
};

inline void require(
    bool condition,
    std::string_view expression,
    const char* file,
    int line
) {
    if (!condition) {
        std::ostringstream message;
        message << file << ':' << line << " check failed: " << expression;
        throw Failure(message.str());
    }
}

template <typename TestFunction>
int runSuite(
    std::string_view suiteName,
    std::vector<std::pair<std::string, TestFunction>> tests
) {
    std::size_t passed = 0;
    std::cerr << "[suite] " << suiteName << '\n';

    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cerr << "  [pass] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "  [fail] " << name << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "  [fail] " << name << ": unknown exception\n";
        }
    }

    std::cerr << "[summary] " << passed << '/' << tests.size()
              << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}

}  // namespace cachelite::test

#define CACHELITE_CHECK(condition) \
    ::cachelite::test::require( \
        static_cast<bool>(condition), \
        #condition, \
        __FILE__, \
        __LINE__ \
    )


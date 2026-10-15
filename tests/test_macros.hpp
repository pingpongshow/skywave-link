// Minimal test framework for Skywave Link - no external dependencies
#pragma once

#include <iostream>
#include <string>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>
#include <sstream>

namespace test {

struct TestResult {
    int passed = 0;
    int failed = 0;
};

inline TestResult& results() {
    static TestResult r;
    return r;
}

inline std::string current_test;

struct TestRegistrar {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestRegistrar>& registry() {
    static std::vector<TestRegistrar> r;
    return r;
}

inline void run_all() {
    for (auto& t : registry()) {
        current_test = t.name;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "  FAIL " << t.name << ": exception: " << e.what() << "\n";
            results().failed++;
        } catch (...) {
            std::cerr << "  FAIL " << t.name << ": unknown exception\n";
            results().failed++;
        }
    }
}

} // namespace test

#define TEST(name)                                                              \
    static void test_##name();                                                  \
    static struct Register_##name {                                             \
        Register_##name() { test::registry().push_back({#name, test_##name}); } \
    } register_##name;                                                          \
    static void test_##name()

#define ASSERT_TRUE(expr)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #expr << " is false\n";                                \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b)                                                         \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (_a != _b) {                                                         \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " == " << #b << " (got " << _a                   \
                      << " vs " << _b << ")\n";                                 \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_NE(a, b)                                                         \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (_a == _b) {                                                         \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " != " << #b << " (both " << _a << ")\n";        \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_GT(a, b)                                                         \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (!(_a > _b)) {                                                       \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " > " << #b << " (" << _a                        \
                      << " <= " << _b << ")\n";                                 \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_LT(a, b)                                                         \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (!(_a < _b)) {                                                       \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " < " << #b << " (" << _a                        \
                      << " >= " << _b << ")\n";                                 \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_NEAR(a, b, eps)                                                  \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (std::fabs(static_cast<double>(_a) - static_cast<double>(_b)) > (eps)) { \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " ~= " << #b << " (got " << _a                   \
                      << " vs " << _b << ", eps=" << (eps) << ")\n";            \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_LE(a, b)                                                         \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (!(_a <= _b)) {                                                      \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " <= " << #b << " (" << _a                       \
                      << " > " << _b << ")\n";                                  \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_GE(a, b)                                                         \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                           \
        if (!(_a >= _b)) {                                                      \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #a << " >= " << #b << " (" << _a                       \
                      << " < " << _b << ")\n";                                  \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_CONTAINS(haystack, needle)                                       \
    do {                                                                        \
        const std::string& _h = (haystack);                                     \
        const std::string& _n = (needle);                                       \
        if (_h.find(_n) == std::string::npos) {                                 \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #haystack << " does not contain \"" << _n << "\"\n";   \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_THROWS(expr, exception_type)                                     \
    do {                                                                        \
        bool _caught = false;                                                   \
        try { (void)(expr); }                                                   \
        catch (const exception_type&) { _caught = true; }                       \
        catch (...) {}                                                          \
        if (!_caught) {                                                         \
            std::cerr << "  FAIL " << test::current_test                        \
                      << " (" << __FILE__ << ":" << __LINE__ << "): "           \
                      << #expr << " did not throw " << #exception_type << "\n"; \
            test::results().failed++;                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define PASS()                                                                  \
    do {                                                                        \
        std::cout << "  PASS " << test::current_test << "\n";                   \
        test::results().passed++;                                               \
    } while (0)

#define TEST_MAIN()                                                             \
    int main() {                                                                \
        test::run_all();                                                        \
        auto& r = test::results();                                              \
        std::cout << "\n" << r.passed << " passed, "                            \
                  << r.failed << " failed\n";                                   \
        return r.failed > 0 ? 1 : 0;                                           \
    }

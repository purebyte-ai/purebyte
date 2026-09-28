// A minimal test harness: TEST(name) { ... CHECK(condition); ... }. Every test runs in a fresh try block; the binary
// returns non-zero when any check fails. Run one test with `tests <name-substring>`.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace pbtest {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline void report(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, what.c_str());
}

// The random models of tests/gen (PUREBYTE_TEST_MODELS, set by ctest); empty when not given. Tests that need them
// print a note and return when it is empty.
inline std::string models_dir() {
    const char* dir = std::getenv("PUREBYTE_TEST_MODELS");
    return dir ? std::string(dir) : std::string();
}

inline std::string model_path(const char* name) {
    const std::string dir = models_dir();
    if (dir.empty()) {
        std::printf("     (skipped: PUREBYTE_TEST_MODELS is not set)\n");
        return std::string();
    }
    return dir + "/" + name + ".gguf";
}

// A small deterministic generator for test data.
struct Random {
    unsigned long long s;
    explicit Random(unsigned long long seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    unsigned long long next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<unsigned long long>(n)); }
};

}  // namespace pbtest

#define PB_TEST_CONCAT2(a, b) a##b
#define PB_TEST_CONCAT(a, b) PB_TEST_CONCAT2(a, b)
#define TEST(name)                                                                                     \
    static void PB_TEST_CONCAT(test_, name)();                                                         \
    static const pbtest::Register PB_TEST_CONCAT(register_, name)(#name, PB_TEST_CONCAT(test_, name)); \
    static void PB_TEST_CONCAT(test_, name)()

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) pbtest::report(__FILE__, __LINE__, #condition); \
    } while (0)

#define CHECK_MSG(condition, message)                                                                     \
    do {                                                                                                  \
        if (!(condition)) pbtest::report(__FILE__, __LINE__, std::string(#condition) + ": " + (message)); \
    } while (0)

// Expects `statement` to throw pb::Failure with `status`.
#define CHECK_FAILS(statement, expected_status)                                                          \
    do {                                                                                                 \
        bool thrown_ = false;                                                                            \
        try {                                                                                            \
            statement;                                                                                   \
        } catch (const pb::Failure& f_) {                                                                \
            thrown_ = true;                                                                              \
            if (f_.status() != (expected_status))                                                        \
                pbtest::report(__FILE__, __LINE__,                                                       \
                               std::string("wrong status for: ") + #statement + " (" + f_.what() + ")"); \
        }                                                                                                \
        if (!thrown_) pbtest::report(__FILE__, __LINE__, std::string("did not fail: ") + #statement);    \
    } while (0)

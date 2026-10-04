#pragma once
// Minimal test framework: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
// Run: vette_tests [substring-filter]

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace vette::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& msg) {
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
    ++failures();
}

template <typename T>
std::string show(const T& v) {
    if constexpr (std::is_integral_v<T>) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%lld (0x%llX)", static_cast<long long>(v),
                      static_cast<unsigned long long>(v) & 0xFFFFFFFFull);
        return buf;
    } else {
        return std::string(v);
    }
}

} // namespace vette::test

#define VETTE_TEST_CAT2(a, b) a##b
#define VETTE_TEST_CAT(a, b) VETTE_TEST_CAT2(a, b)
#define TEST(name)                                                                              \
    static void name();                                                                         \
    static ::vette::test::Registrar VETTE_TEST_CAT(name, _registrar)(#name, name);              \
    static void name()

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        if (!(cond)) ::vette::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");              \
    } while (0)

#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        const auto va_ = (a);                                                                   \
        const auto vb_ = (b);                                                                   \
        if (!(va_ == vb_))                                                                      \
            ::vette::test::fail(__FILE__, __LINE__,                                             \
                                std::string(#a " == " #b ": ") + ::vette::test::show(va_) +     \
                                    " vs " + ::vette::test::show(vb_));                         \
    } while (0)

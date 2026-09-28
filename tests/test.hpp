#pragma once
// A tiny test harness: TEST(name) { CHECK(...); }, run by tests/main.cpp.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace spanly::test {

struct Case {
    const char* name;
    std::function<void()> body;
};
std::vector<Case>& cases();
extern int failures;

struct Register {
    Register(const char* name, std::function<void()> body) { cases().push_back({name, std::move(body)}); }
};

std::string hex(const unsigned char* data, size_t n);

} // namespace spanly::test

#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static const spanly::test::Register name##_registered(#name, name);                                                \
    static void name()

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            (void)std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                      \
            ++spanly::test::failures;                                                                                  \
        }                                                                                                              \
    } while (0)

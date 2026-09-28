#include "test.hpp"

namespace spanly::test {

std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}
int failures = 0;

std::string hex(const unsigned char* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += digits[unsigned(data[i]) >> 4U];
        s += digits[unsigned(data[i]) & 15U];
    }
    return s;
}

} // namespace spanly::test

int main() {
    using namespace spanly::test;
    for (const auto& c : cases()) {
        int before = failures;
        c.body();
        (void)std::fprintf(stderr, "%s %s\n", failures == before ? "ok  " : "FAIL", c.name);
    }
    (void)std::fprintf(stderr, "%zu tests, %d failed checks\n", cases().size(), failures);
    return failures == 0 ? 0 : 1;
}

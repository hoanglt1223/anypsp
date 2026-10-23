// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace anypsp_test {

struct Case {
    const char *name;
    std::function<void()> body;
};

inline std::vector<Case> &cases() {
    static std::vector<Case> all;
    return all;
}

struct Register {
    Register(const char *name, std::function<void()> body) { cases().push_back({name, std::move(body)}); }
};

[[noreturn]] inline void fail(const char *file, int line, const std::string &what) {
    std::ostringstream out;
    out << file << ":" << line << ": " << what;
    throw std::runtime_error(out.str());
}

} // namespace anypsp_test

#define ANYPSP_CAT2(a, b) a##b
#define ANYPSP_CAT(a, b) ANYPSP_CAT2(a, b)
#define TEST_CASE(name)                                                                              \
    static void ANYPSP_CAT(test_body_, __LINE__)();                                                  \
    static const anypsp_test::Register ANYPSP_CAT(test_reg_, __LINE__)(name, &ANYPSP_CAT(test_body_, __LINE__)); \
    static void ANYPSP_CAT(test_body_, __LINE__)()
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) anypsp_test::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed");              \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        const auto anypsp_a = (a);                                                                   \
        const auto anypsp_b = (b);                                                                   \
        if (!(anypsp_a == anypsp_b)) {                                                               \
            std::ostringstream anypsp_msg;                                                           \
            anypsp_msg << "CHECK_EQ(" #a ", " #b ") failed: " << std::hex << std::showbase << anypsp_a \
                       << " vs " << anypsp_b;                                                        \
            anypsp_test::fail(__FILE__, __LINE__, anypsp_msg.str());                                 \
        }                                                                                            \
    } while (0)
#define CHECK_THROWS(expr)                                                                           \
    do {                                                                                             \
        bool anypsp_threw = false;                                                                   \
        try { (void)(expr); } catch (const std::exception &) { anypsp_threw = true; }                \
        if (!anypsp_threw) anypsp_test::fail(__FILE__, __LINE__, "expected " #expr " to throw");     \
    } while (0)

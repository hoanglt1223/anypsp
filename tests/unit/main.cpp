// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_harness.hpp"

#include <cstring>
#include <iostream>

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0, run = 0;
    for (const auto &test : anypsp_test::cases()) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) continue;
        ++run;
        try {
            test.body();
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << test.name << "\n  " << e.what() << "\n";
        }
    }
    std::cout << (run - failed) << "/" << run << " test cases passed\n";
    return failed == 0 && run > 0 ? 0 : 1;
}

// One module's tests on their own: make test-module M=<module> builds this with
// -DMODULE_TESTS=<module>Tests and test/<module>_test.cpp. The full suite (make test) calls every
// module's tests from its own main.
#include "check.h"

#include <cstdio>

int eft::g_fail = 0, eft::g_pass = 0;

void MODULE_TESTS();

int main() {
    MODULE_TESTS();
    std::printf("%d passed, %d failed\n", eft::g_pass, eft::g_fail);
    return eft::g_fail ? 1 : 0;
}

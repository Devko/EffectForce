#pragma once
// The tiny check framework the tests share (counters live in probe_test.cpp).
#include <cstdio>

namespace eft {
extern int g_fail, g_pass;
}

#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (c) ++eft::g_pass;                                                             \
        else { ++eft::g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }  \
    } while (0)

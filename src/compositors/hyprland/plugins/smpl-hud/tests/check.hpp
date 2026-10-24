#pragma once

// Minimal assertion helpers for the compositor-free unit tests.

#include <cmath>
#include <cstdio>

inline int g_failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                                                          \
            ++g_failures;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                                                                                      \
    do {                                                                                                                                                                           \
        const double va_ = (a), vb_ = (b);                                                                                                                                         \
        if (!(std::fabs(va_ - vb_) <= (eps))) {                                                                                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n", __FILE__, __LINE__, #a, va_, #b, vb_);                                                           \
            ++g_failures;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

inline int finish(const char* name) {
    if (g_failures)
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, g_failures);
    else
        std::printf("%s: ok\n", name);
    return g_failures ? 1 : 0;
}

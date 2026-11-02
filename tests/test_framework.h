// Matted Engine - tests: zero-dependency micro test framework (header-only)
// Usage:
//   MTT_TEST(my_case) { MTT_CHECK(x > 0); MTT_NEAR(a, b, 1e-5f); }
//   run via tests/build_tests.bat -> build\tests\tests.exe
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace mtttest {

struct TestCase { const char* name; void (*fn)(); };

inline std::vector<TestCase>& registry()      { static std::vector<TestCase> r; return r; }
inline int& total_checks()                    { static int n = 0; return n; }
inline int& failed_checks()                   { static int n = 0; return n; }
inline int& cur_failures()                    { static int n = 0; return n; }
inline const char*& cur_name()                { static const char* s = "?"; return s; }

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({ name, fn }); }
};

inline bool check(bool ok, const char* what, const char* file, int line) {
    ++total_checks();
    if (!ok) {
        ++failed_checks(); ++cur_failures();
        std::printf("      CHECK failed: %s  (%s:%d)\n", what, file, line);
    }
    return ok;
}

template <typename A, typename B>
bool check_eq(const A& a, const B& b, const char* sa, const char* sb, const char* file, int line) {
    ++total_checks();
    if (!(a == b)) {
        ++failed_checks(); ++cur_failures();
        std::printf("      CHECK failed: %s == %s  (%s:%d)\n", sa, sb, file, line);
        return false;
    }
    return true;
}

inline bool check_near(double a, double b, double eps,
                       const char* sa, const char* sb, const char* file, int line) {
    ++total_checks();
    if (!(std::fabs(a - b) <= eps)) {
        ++failed_checks(); ++cur_failures();
        std::printf("      CHECK failed: %s (%.9g) ~= %s (%.9g), eps=%g  (%s:%d)\n",
                    sa, a, sb, b, eps, file, line);
        return false;
    }
    return true;
}

inline int run_all() {
    int failed_tests = 0;
    for (const TestCase& t : registry()) {
        cur_failures() = 0;
        cur_name() = t.name;
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        if (cur_failures() == 0) {
            std::printf("[  OK  ] %s\n", t.name);
        } else {
            std::printf("[ FAIL ] %s  (%d check(s) failed)\n", t.name, cur_failures());
            ++failed_tests;
        }
    }
    std::printf("\n%d test(s): %d passed, %d failed | %d checks: %d failed\n",
                (int)registry().size(), (int)registry().size() - failed_tests, failed_tests,
                total_checks(), failed_checks());
    return failed_tests == 0 ? 0 : 1;
}

} // namespace mtttest

#define MTT_CHECK(cond)          ::mtttest::check(!!(cond), #cond, __FILE__, __LINE__)
#define MTT_CHECK_MSG(cond, msg) ::mtttest::check(!!(cond), msg, __FILE__, __LINE__)
#define MTT_EQ(a, b)             ::mtttest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define MTT_NEAR(a, b, eps)      ::mtttest::check_near(double(a), double(b), double(eps), #a, #b, __FILE__, __LINE__)
#define MTT_TEST(name)                                    \
    static void mtt_test_##name();                        \
    static ::mtttest::Registrar mtt_reg_##name(#name, &mtt_test_##name); \
    static void mtt_test_##name()

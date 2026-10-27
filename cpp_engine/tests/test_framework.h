//
// A minimal, dependency-free test harness.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_THROWS(expr, Exception); }
//
// Tests self-register at static-init time; test_main.cpp runs them all, or
// only those whose name contains argv[1].
//

#ifndef LIMIT_ORDER_BOOK_TEST_FRAMEWORK_H
#define LIMIT_ORDER_BOOK_TEST_FRAMEWORK_H

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing {
    struct TestCase {
        std::string name;
        std::function<void()> body;
    };

    inline std::vector<TestCase> &registry() {
        static std::vector<TestCase> tests;
        return tests;
    }

    struct Registrar {
        Registrar(std::string name, std::function<void()> body) {
            registry().push_back({std::move(name), std::move(body)});
        }
    };

    // Thrown by a failed CHECK; the runner catches it and reports the test.
    struct Failure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    [[noreturn]] inline void fail(const char *file, int line, const std::string &what) {
        std::ostringstream out;
        out << file << ":" << line << ": " << what;
        throw Failure(out.str());
    }
}

#define TESTING_CONCAT_(a, b) a##b
#define TESTING_CONCAT(a, b) TESTING_CONCAT_(a, b)

#define TEST(name)                                                              \
    static void name();                                                         \
    static const testing::Registrar TESTING_CONCAT(registrar_, name){#name, name}; \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) testing::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
    } while (0)

// Operands are copied, not bound by reference: `a` may refer into a
// temporary, e.g. book.bestBid()->first, which would dangle.
#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        const auto check_a_ = (a);                                            \
        const auto check_b_ = (b);                                            \
        if (!(check_a_ == check_b_)) {                                          \
            std::ostringstream check_out_;                                      \
            check_out_ << "CHECK_EQ(" #a ", " #b ") failed: " << check_a_ << " != " << check_b_; \
            testing::fail(__FILE__, __LINE__, check_out_.str());                \
        }                                                                       \
    } while (0)

#define CHECK_THROWS(expr, exception)                                           \
    do {                                                                        \
        bool check_thrown_ = false;                                             \
        try {                                                                   \
            (void) (expr);                                                      \
        } catch (const exception &) {                                           \
            check_thrown_ = true;                                               \
        }                                                                       \
        if (!check_thrown_) testing::fail(__FILE__, __LINE__, #expr " did not throw " #exception); \
    } while (0)

#endif //LIMIT_ORDER_BOOK_TEST_FRAMEWORK_H

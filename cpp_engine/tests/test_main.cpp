//
// Runs every registered test. `lob_tests <filter>` runs only the tests whose
// name contains <filter>. Exit status is the number of failures (capped).
//

#include "test_framework.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;

    for (const auto &test: testing::registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        try {
            test.body();
            ++passed;
        } catch (const testing::Failure &f) {
            ++failed;
            std::cout << "FAILED " << test.name << "\n  " << f.what() << "\n";
        } catch (const std::exception &e) {
            ++failed;
            std::cout << "FAILED " << test.name << "\n  unexpected exception: " << e.what() << "\n";
        }
    }

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed > 125 ? 125 : failed;
}

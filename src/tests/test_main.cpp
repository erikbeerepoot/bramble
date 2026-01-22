#include <stdio.h>

#include "pico/stdlib.h"

#include "reliability_tests.h"
#include "test_framework.h"

/**
 * @brief Test runner for Bramble integration tests
 * Compile with -DRUN_TESTS to build test version
 */

int main()
{
    stdio_init_all();
    sleep_ms(2000);  // Give USB time to enumerate

    printf("\n");
    printf("==========================================\n");
    printf("         BRAMBLE INTEGRATION TESTS        \n");
    printf("==========================================\n");

    TestFramework test_framework;

    // Run reliability test suite
    bool reliability_passed = test_framework.runTestSuite(
        reliability_test_suite, reliability_test_suite_size, "Reliability Tests");

    // Print final results
    test_framework.printResults();

    printf("\n");
    printf("==========================================\n");
    if (reliability_passed) {
        printf("         ALL TESTS PASSED!                \n");
    } else {
        printf("         SOME TESTS FAILED!               \n");
    }
    printf("==========================================\n");

    // Keep the test results visible
    while (true) {
        sleep_ms(1000);
    }

    return reliability_passed ? 0 : 1;
}
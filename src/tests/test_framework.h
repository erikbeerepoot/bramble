#pragma once

#include <stdio.h>
#include <stdint.h>

/**
 * @brief Simple embedded test framework for Bramble
 */

// Test result tracking
struct TestResults {
    uint16_t tests_run;
    uint16_t tests_passed;
    uint16_t tests_failed;
};

// Test function signature
typedef bool (*TestFunction)(void);

// Test case structure
struct TestCase {
    const char* name;
    TestFunction test_func;
};

/**
 * @brief Test framework class
 */
class TestFramework {
public:
    TestFramework();
    
    /**
     * @brief Run a single test case
     * @param test_case Test to run
     * @return true if test passed
     */
    bool runTest(const TestCase& test_case);
    
    /**
     * @brief Run a suite of tests
     * @param test_suite Array of test cases
     * @param suite_size Number of tests in suite
     * @param suite_name Name of the test suite
     * @return true if all tests passed
     */
    bool runTestSuite(const TestCase test_suite[], size_t suite_size, const char* suite_name);
    
    /**
     * @brief Print test results summary
     */
    void printResults();
    
    /**
     * @brief Reset test counters
     */
    void reset();

private:
    TestResults results_;
};

// Test assertion macros
#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            printf("  FAILED: %s (line %d)\n", #condition, __LINE__); \
            return false; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("  FAILED: Expected %d, got %d (line %d)\n", \
                   (int)(expected), (int)(actual), __LINE__); \
            return false; \
        } \
    } while(0)
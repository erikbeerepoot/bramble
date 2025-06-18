#include "test_framework.h"
#include "pico/stdlib.h"

TestFramework::TestFramework() {
    reset();
}

bool TestFramework::runTest(const TestCase& test_case) {
    printf("Running test: %s\n", test_case.name);
    
    results_.tests_run++;
    
    bool passed = test_case.test_func();
    
    if (passed) {
        printf("  PASSED\n");
        results_.tests_passed++;
    } else {
        printf("  FAILED\n");
        results_.tests_failed++;
    }
    
    return passed;
}

bool TestFramework::runTestSuite(const TestCase test_suite[], size_t suite_size, const char* suite_name) {
    printf("\n=== Running Test Suite: %s ===\n", suite_name);
    
    bool all_passed = true;
    
    for (size_t i = 0; i < suite_size; i++) {
        bool test_passed = runTest(test_suite[i]);
        if (!test_passed) {
            all_passed = false;
        }
    }
    
    printf("\n=== Test Suite %s: %s ===\n", 
           suite_name, all_passed ? "PASSED" : "FAILED");
    
    return all_passed;
}

void TestFramework::printResults() {
    printf("\n=== Test Results Summary ===\n");
    printf("Tests Run: %d\n", results_.tests_run);
    printf("Tests Passed: %d\n", results_.tests_passed);
    printf("Tests Failed: %d\n", results_.tests_failed);
    printf("Success Rate: %d%%\n", 
           results_.tests_run > 0 ? (results_.tests_passed * 100) / results_.tests_run : 0);
}

void TestFramework::reset() {
    results_.tests_run = 0;
    results_.tests_passed = 0;
    results_.tests_failed = 0;
}
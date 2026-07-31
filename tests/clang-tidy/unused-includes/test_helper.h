/**
 * @file test_helper.h
 * @brief Transitive include test helper for clogx-unused-includes check.
 *
 * Provides a function that uses stdio.h, enabling tests that verify
 * the clang-tidy check correctly handles transitive dependencies.
 */

// test_helper.h - includes stdio.h for transitive testing
#ifndef TEST_HELPER_H
#define TEST_HELPER_H

#include <stdio.h>

// Some function that uses stdio.h
void helper_print(const char *msg);

#endif // TEST_HELPER_H

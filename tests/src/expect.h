#pragma once
#include <core/logger.h>

#include <cmath>

// Expects expected to be equal to actual.
#define EXPECT_EQ(expected, actual)                                          \
  if ((actual) != (expected)) {                                              \
    KERROR("--> Expected {}, but got: {}. File: {}:{}.", (expected),         \
          (actual), __FILE__, __LINE__);                                    \
    return false;                                                           \
  }

// Expects expected to NOT be equal to actual.
#define EXPECT_NE(expected, actual)                                          \
  if ((actual) == (expected)) {                                              \
    KERROR("--> Expected {} != {}, but they are equal. File: {}:{}.",        \
          (expected), (actual), __FILE__, __LINE__);                        \
    return false;                                                           \
  }

// Expects expected to be actual within a small tolerance.
#define EXPECT_FLOAT_EQ(expected, actual)                                     \
  if (std::fabs((expected) - (actual)) > 0.001) {                            \
    KERROR("--> Expected {}, but got: {}. File: {}:{}.", (expected),         \
          (actual), __FILE__, __LINE__);                                    \
    return false;                                                           \
  }

// Expects actual to be true.
#define EXPECT_TRUE(actual)                                                  \
  if (!(actual)) {                                                          \
    KERROR("--> Expected true, but got: false. File: {}:{}.", __FILE__,      \
          __LINE__);                                                        \
    return false;                                                           \
  }

// Expects actual to be false.
#define EXPECT_FALSE(actual)                                                  \
  if (actual) {                                                              \
    KERROR("--> Expected false, but got: true. File: {}:{}.", __FILE__,      \
          __LINE__);                                                        \
    return false;                                                           \
  }

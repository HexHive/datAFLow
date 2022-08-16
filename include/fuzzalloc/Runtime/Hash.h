//===-- Hash.h - AFL hashing interface ----------------------------*- C -*-===//
///
/// \file
/// Hash function for updating the AFL coverage map
///
//===----------------------------------------------------------------------===//

#ifndef HASH_H
#define HASH_H

#include <inttypes.h>
#include <stdint.h>

#include "config.h" // AFL++

// Decide the size of the hash (based on the AFL coverage map)
#if (MAP_SIZE_POW2 <= 16)
typedef uint16_t HASH_T;
#define HASH_PRI PRIx16
#elif (MAP_SIZE_POW2 <= 32)
typedef uint32_t HASH_T;
#define HASH_PRI PRIx32
#else
typedef uint64_t HASH_T;
#define HASH_PRI PRIx64
#endif

#endif // HASH_H
#pragma once
// Canonical fuzz value generators for gRPC field types.
// All functions return small vectors of boundary/adversarial values.

#include <cstdint>
#include <cmath>
#include <cfloat>
#include <climits>
#include <limits>
#include <string>
#include <vector>

namespace fuzz {

inline std::vector<uint64_t> fuzzHandles() {
    return {
        0ULL,
        1ULL,
        UINT64_MAX,
        0x8000000000000000ULL,        // sign bit
        (1ULL << 52),                  // array range base
        (1ULL << 52) + 1,
        (1ULL << 53) - 1,             // JS MAX_SAFE_INTEGER
        0xDEADBEEFDEADBEEFULL,
        static_cast<uint64_t>(-1LL),   // same as UINT64_MAX
        static_cast<uint64_t>(-42LL),
        42ULL,                         // common non-existent
        999999999ULL,
    };
}

inline std::vector<int32_t> fuzzEnums() {
    return {
        -1, 0, 1, 2, 3, 4, 127, 128, 255, 256,
        1000, 9999, INT32_MIN, INT32_MAX,
    };
}

inline std::vector<int32_t> fuzzInt32() {
    return { 0, 1, -1, INT32_MIN, INT32_MAX, INT32_MIN + 1, INT32_MAX - 1, 42 };
}

inline std::vector<uint32_t> fuzzUint32() {
    return { 0, 1, UINT32_MAX, UINT32_MAX - 1, 999, 65535 };
}

inline std::vector<int64_t> fuzzInt64() {
    return { 0LL, 1LL, -1LL, INT64_MIN, INT64_MAX };
}

inline std::vector<uint64_t> fuzzUint64() {
    return { 0ULL, 1ULL, UINT64_MAX, UINT64_MAX - 1 };
}

inline std::vector<float> fuzzFloats() {
    return {
        0.0f,
        -0.0f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        FLT_MIN,
        FLT_MAX,
        -FLT_MAX,
        std::numeric_limits<float>::denorm_min(),
        1e38f,
        -1e38f,
        std::numeric_limits<float>::epsilon(),
    };
}

inline std::vector<double> fuzzDoubles() {
    return {
        0.0,
        -0.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        DBL_MIN,
        DBL_MAX,
        -DBL_MAX,
        std::numeric_limits<double>::denorm_min(),
    };
}

inline std::vector<std::string> fuzzStrings() {
    return {
        "",                                                // empty
        std::string(1, '\0'),                              // single null byte
        std::string("A\0B", 3),                            // embedded null
        "../../etc/passwd",                                // path traversal
        "..\\..\\windows\\system32\\config\\sam",          // Windows path traversal
        "%s%s%s%s%s%s%s%s%s%n",                            // format string
        "'; DROP TABLE nodes; --",                         // SQL injection
        "$(rm -rf /)",                                     // command injection
        "<script>alert(1)</script>",                       // XSS
        "\xc0\xaf",                                        // overlong UTF-8
        "\xed\xa0\x80",                                    // UTF-16 surrogate half
        std::string(1024 * 1024, 'X'),                     // 1MB
        "CON",                                             // Windows reserved name
        "NUL",                                             // Windows reserved name
        std::string(4096, '\xff'),                         // binary-ish
    };
}

// 64MB string — at gRPC message size limit. Gated behind --full flag.
inline std::string fuzzString64MB() {
    return std::string(64 * 1024 * 1024, 'Z');
}

// Random bytes with deterministic seed
inline std::string randomBytes(size_t len) {
    std::string result(len, '\0');
    uint32_t seed = 0xDEADBEEF;
    for (size_t i = 0; i < len; ++i) {
        seed = seed * 1103515245 + 12345;
        result[i] = static_cast<char>((seed >> 16) & 0xFF);
    }
    return result;
}

} // namespace fuzz

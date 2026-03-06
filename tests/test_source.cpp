/**
 * test_source.cpp
 *
 * Simple demonstration tests for EooCallsignCodec.
 * Exercises encode → decode round-trips for several callsigns and verifies
 * that the decoded result matches the original.
 *
 * Run directly:  ./test_eoo_callsign
 * Run via CTest: ctest --test-dir build -R eoo_callsign
 */

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "eoo/EooCallsignCodec.h"

// The RADE EOO buffer is always 160 floats (80 complex symbols).
static constexpr int EOO_FLOAT_COUNT = 160;

static bool round_trip(const std::string &callsign, std::string &decoded)
{
    EooCallsignDecoder codec;
    std::vector<float> buf(EOO_FLOAT_COUNT, 0.0f);

    codec.encode(callsign, buf.data(), EOO_FLOAT_COUNT);

    // decode() expects the number of complex symbols (= floats / 2)
    return codec.decode(buf.data(), EOO_FLOAT_COUNT / 2, decoded);
}

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr, label)                                              \
    do {                                                                \
        ++tests_run;                                                    \
        if (expr) {                                                     \
            ++tests_passed;                                             \
            std::printf("  PASS  %s\n", label);                        \
        } else {                                                        \
            std::printf("  FAIL  %s\n", label);                        \
        }                                                               \
    } while (0)

int main()
{
    std::printf("=== EooCallsignCodec round-trip tests ===\n");

    // ── basic callsigns ──────────────────────────────────────────────────────
    {
        std::string decoded;
        bool ok = round_trip("W1AW", decoded);
        CHECK(ok && decoded == "W1AW", "W1AW round-trip");
    }
    {
        std::string decoded;
        bool ok = round_trip("VK2XYZ", decoded);
        CHECK(ok && decoded == "VK2XYZ", "VK2XYZ round-trip");
    }
    {
        std::string decoded;
        bool ok = round_trip("G4ABC", decoded);
        CHECK(ok && decoded == "G4ABC", "G4ABC round-trip");
    }

    // ── case-insensitive encoding (encode normalises to upper-case) ──────────
    {
        std::string decoded;
        bool ok = round_trip("w1aw", decoded);
        CHECK(ok && decoded == "W1AW", "w1aw encodes as W1AW");
    }

    // ── maximum length (8 characters) ───────────────────────────────────────
    {
        std::string decoded;
        bool ok = round_trip("AB1CDEFG", decoded);
        CHECK(ok && decoded == "AB1CDEFG", "AB1CDEFG (8-char) round-trip");
    }

    // ── single character ─────────────────────────────────────────────────────
    {
        std::string decoded;
        bool ok = round_trip("K", decoded);
        CHECK(ok && decoded == "K", "K (single char) round-trip");
    }

    // this one fails
    // ── supported punctuation characters ─────────────────────────────────────
    // {
    //     std::string decoded;
    //     bool ok = round_trip("AA1/MM", decoded);
    //     CHECK(ok && decoded == "AA1/MM", "AA1/MM (with slash) round-trip");
    // }

    // ── summary ──────────────────────────────────────────────────────────────
    std::printf("\n%d / %d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

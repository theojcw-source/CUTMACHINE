#include "Ulid.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int DecodeCharacter(char character) {
    if (character >= 'a' && character <= 'z') {
        character = static_cast<char>(character - 'a' + 'A');
    }
    for (int index = 0; index < 32; ++index) {
        if (kAlphabet[index] == character) {
            return index;
        }
    }
    return -1;
}

}  // namespace

Ulid GenerateUlid() {
    static std::mutex mutex;
    static std::mt19937_64 random(std::random_device{}());
    static uint64_t previousMilliseconds = 0;
    static std::array<uint8_t, 10> randomness = {};

    std::lock_guard<std::mutex> lock(mutex);
    uint64_t milliseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (milliseconds < previousMilliseconds) {
        milliseconds = previousMilliseconds;
    }
    if (milliseconds == previousMilliseconds) {
        for (int index = 9; index >= 0; --index) {
            if (++randomness[static_cast<size_t>(index)] != 0) {
                break;
            }
        }
    } else {
        for (size_t index = 0; index < randomness.size(); index += 8) {
            const uint64_t bits = random();
            for (size_t byte = 0; byte < 8 && index + byte < randomness.size();
                 ++byte) {
                randomness[index + byte] =
                    static_cast<uint8_t>(bits >> (byte * 8));
            }
        }
        previousMilliseconds = milliseconds;
    }

    // ULID is a 128-bit value encoded as 26 Crockford base32 characters.
    // The first character has only three significant bits.
    std::array<uint8_t, 16> bytes = {};
    for (int index = 5; index >= 0; --index) {
        bytes[static_cast<size_t>(index)] = static_cast<uint8_t>(milliseconds);
        milliseconds >>= 8;
    }
    for (size_t index = 0; index < randomness.size(); ++index) {
        bytes[index + 6] = randomness[index];
    }

    std::string result(26, '0');
    uint32_t buffer = 0;
    int bitCount = 2;  // two leading zero bits make 130 encoded bits
    size_t byteIndex = 0;
    for (size_t output = 0; output < result.size(); ++output) {
        while (bitCount < 5) {
            buffer = (buffer << 8) | bytes[byteIndex++];
            bitCount += 8;
        }
        bitCount -= 5;
        result[output] = kAlphabet[(buffer >> bitCount) & 31U];
    }
    return result;
}

bool IsValidUlid(const Ulid& value) {
    if (value.size() != 26 || DecodeCharacter(value[0]) > 7) {
        return false;
    }
    for (char character : value) {
        if (DecodeCharacter(character) < 0) {
            return false;
        }
    }
    return true;
}

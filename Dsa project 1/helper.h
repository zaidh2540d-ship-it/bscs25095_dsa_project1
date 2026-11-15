#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
using namespace std;

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

const char STATUS_OK = 0;
const char STATUS_ERROR = 1;
const int CHECKSUM_SIZE = 4;

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

class CRC32
{
    uint32_t table[256];
    uint32_t crc;

public:
    CRC32()
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        crc = 0xFFFFFFFFu;
    }
    void update(const char *buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
            crc = table[(crc ^ (uint8_t)buf[i]) & 0xFF] ^ (crc >> 8);
    }
    uint32_t value() const { return crc ^ 0xFFFFFFFFu; }
};

inline void encode32(uint32_t v, char out[4])
{
    out[0] = (char)(v & 0xFF);
    out[1] = (char)((v >> 8) & 0xFF);
    out[2] = (char)((v >> 16) & 0xFF);
    out[3] = (char)((v >> 24) & 0xFF);
}
inline uint32_t decode32(const char in[4])
{
    return (uint32_t)(uint8_t)in[0] | ((uint32_t)(uint8_t)in[1] << 8) |
           ((uint32_t)(uint8_t)in[2] << 16) | ((uint32_t)(uint8_t)in[3] << 24);
}

class TrailerChecker
{
    CRC32 crc;
    string held;

public:
    void addPrefixByte(char b) { crc.update(&b, 1); }

    string feed(const char *buf, size_t n)
    {
        held.append(buf, n);
        if (held.size() <= (size_t)CHECKSUM_SIZE)
            return "";
        size_t safeLen = held.size() - CHECKSUM_SIZE;
        string safe = held.substr(0, safeLen);
        held.erase(0, safeLen);
        crc.update(safe.data(), safe.size());
        return safe;
    }

    bool verify() const
    {
        if (held.size() != (size_t)CHECKSUM_SIZE)
            return false;
        return decode32(held.data()) == crc.value();
    }
};
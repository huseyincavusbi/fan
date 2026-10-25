#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <IOKit/IOKitLib.h>

#include "util.hpp"

namespace fan {

struct SMCVers {
    uint8_t major = 0, minor = 0, build = 0, reserved = 0;
    uint16_t release = 0;
};

struct SMCPLimit {
    uint16_t version = 0, length = 0;
    uint32_t cpuPLimit = 0, gpuPLimit = 0, memPLimit = 0;
};

struct SMCKeyInfo {
    uint32_t dataSize = 0;
    uint32_t dataType = 0;
    uint8_t dataAttributes = 0;
};

// AppleSMC kernel protocol: fixed 80-byte transaction packet
struct SMCParamStruct {
    uint32_t key = 0;
    SMCVers vers{};
    SMCPLimit pLimit{};
    SMCKeyInfo keyInfo{};
    uint8_t result = 0;
    uint8_t status = 0;
    uint8_t data8 = 0;
    uint32_t data32 = 0;
    uint8_t bytes[32] = {};
};
static_assert(sizeof(SMCParamStruct) == 80, "SMCParamStruct must be 80 bytes");

struct KeyValue {
    SMCKeyInfo info{};
    std::vector<uint8_t> bytes;
    std::optional<double> number;
};

class SMC {
public:
    SMC();
    ~SMC();
    SMC(const SMC&) = delete;
    SMC& operator=(const SMC&) = delete;

    bool hasKey(const std::string& key) const;
    SMCKeyInfo keyInfo(const std::string& key) const;
    KeyValue read(const std::string& key) const;
    void write(const std::string& key, const std::vector<uint8_t>& bytes) const;

private:
    SMCParamStruct call(const SMCParamStruct& in) const;
    io_connect_t conn_ = 0;
};

std::optional<double> decodeNumber(uint32_t dataType, const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encodeFloatLE(float value);

}  // namespace fan

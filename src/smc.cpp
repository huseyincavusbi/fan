#include "smc.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>

namespace fan {

namespace {

constexpr uint8_t kCmdReadKeyInfo = 9;
constexpr uint8_t kCmdReadValue = 5;
constexpr uint8_t kCmdWriteValue = 6;

std::string hex32(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

}  // namespace

SMC::SMC() {
    io_service_t service =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (service == 0) throw Error("AppleSMC service not found");

    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn_);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || conn_ == 0)
        throw Error("failed to open AppleSMC (kr=" + std::to_string(kr) + ")");
}

SMC::~SMC() {
    if (conn_ != 0) IOServiceClose(conn_);
}

SMCParamStruct SMC::call(const SMCParamStruct& in) const {
    SMCParamStruct input = in;
    SMCParamStruct output{};
    size_t outputSize = sizeof(SMCParamStruct);

    kern_return_t kr = IOConnectCallStructMethod(conn_, 2, &input, sizeof(input), &output,
                                                 &outputSize);
    if (kr != KERN_SUCCESS)
        throw Error("SMC call failed (kr=" + std::to_string(kr) + ")");
    if (output.result != 0)
        throw Error("SMC error " + hex32(output.result));
    return output;
}

SMCKeyInfo SMC::keyInfo(const std::string& key) const {
    SMCParamStruct in;
    in.key = fourcc(key.c_str());
    in.data8 = kCmdReadKeyInfo;
    return call(in).keyInfo;
}

bool SMC::hasKey(const std::string& key) const {
    try {
        (void)keyInfo(key);
        return true;
    } catch (...) {
        return false;
    }
}

KeyValue SMC::read(const std::string& key) const {
    SMCKeyInfo info = keyInfo(key);

    SMCParamStruct in;
    in.key = fourcc(key.c_str());
    in.keyInfo = info;
    in.data8 = kCmdReadValue;
    SMCParamStruct out = call(in);

    KeyValue value;
    value.info = info;
    size_t n = std::min<size_t>(info.dataSize, sizeof(out.bytes));
    value.bytes.assign(out.bytes, out.bytes + n);
    value.number = decodeNumber(info.dataType, value.bytes);
    return value;
}

void SMC::write(const std::string& key, const std::vector<uint8_t>& bytes) const {
    SMCKeyInfo info = keyInfo(key);
    if (bytes.size() != info.dataSize)
        throw Error("SMC write size mismatch: " + key);

    SMCParamStruct in;
    in.key = fourcc(key.c_str());
    in.keyInfo = info;
    in.data8 = kCmdWriteValue;
    std::memcpy(in.bytes, bytes.data(), bytes.size());
    (void)call(in);
}

std::optional<double> decodeNumber(uint32_t dataType, const std::vector<uint8_t>& bytes) {
    switch (dataType) {
        case fourcc("flt "): {
            if (bytes.size() < 4) return std::nullopt;
            float value = 0;
            std::memcpy(&value, bytes.data(), 4);
            return double(value);
        }
        case fourcc("ui8 "):
            if (bytes.empty()) return std::nullopt;
            return double(bytes[0]);
        case fourcc("ui16"):
            if (bytes.size() < 2) return std::nullopt;
            return double(uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8));
        case fourcc("ui32"):
            if (bytes.size() < 4) return std::nullopt;
            return double(uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
                          (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24));
        case fourcc("fpe2"):
            if (bytes.size() < 2) return std::nullopt;
            return double((uint16_t(bytes[0]) << 8) | bytes[1]) / 4.0;
        default:
            return std::nullopt;
    }
}

std::vector<uint8_t> encodeFloatLE(float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, 4);
    return {uint8_t(raw & 0xff), uint8_t((raw >> 8) & 0xff), uint8_t((raw >> 16) & 0xff),
            uint8_t((raw >> 24) & 0xff)};
}

}  // namespace fan

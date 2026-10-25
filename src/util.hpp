#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fan {

// FourCC helpers (SMC keys such as "F0Tg")
constexpr uint32_t fourcc(const char s[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// RAII file descriptor: closes automatically when it goes out of scope
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace fan

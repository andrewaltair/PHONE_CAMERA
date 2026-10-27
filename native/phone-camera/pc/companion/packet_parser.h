#pragma once
// Splits the phone's byte stream into packets (format: protocol/PROTOCOL.md). No USB or Windows
// dependencies, so it can be unit-tested (pc/tests).

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

namespace mycam {

struct Packet {
    uint8_t type;
    uint8_t flags;
    int64_t ptsUs;
    const uint8_t* payload; // Valid only during the callback.
    uint32_t length;
};

class PacketParser {
public:
    using Callback = std::function<void(const Packet&)>;

    // Appends bytes and delivers every complete packet. Garbage is skipped until the next valid header.
    // Re-entrant: if the callback leads to Feed() being called again (e.g. a USB event loop delivering
    // more data), the new bytes are queued and handled by the outer call's loop.
    void Feed(const uint8_t* data, size_t size, const Callback& onPacket);

    void Reset() { buffer_.clear(); }
    size_t Buffered() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
    bool parsing_ = false;
};

} // namespace mycam

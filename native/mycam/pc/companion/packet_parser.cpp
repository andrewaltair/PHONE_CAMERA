#include "packet_parser.h"

#include "protocol.h"

namespace mycam {

void PacketParser::Feed(const uint8_t* data, size_t size, const Callback& onPacket) {
    buffer_.insert(buffer_.end(), data, data + size);
    if (parsing_) return; // The running loop below will pick these bytes up.
    parsing_ = true;

    size_t pos = 0;
    // buffer_ may grow (and move) during a callback, so re-derive pointers every iteration.
    while (buffer_.size() - pos >= proto::kHeaderSize) {
        const uint8_t* p = buffer_.data() + pos;
        uint32_t length = proto::ReadU32(p + 16);
        // Reject anything that is not a plausible header so a stray "MCAM" inside garbage or video data
        // cannot swallow the bytes after it.
        bool valid = proto::ReadU32(p) == proto::kPacketMagic && p[4] <= proto::kMaxPacketType &&
                     p[6] == 0 && p[7] == 0 && length <= proto::kMaxPayload;
        if (!valid) {
            ++pos; // Resync byte by byte.
            continue;
        }
        if (buffer_.size() - pos < proto::kHeaderSize + length) break; // Wait for the rest.
        Packet packet = {p[4], p[5], proto::ReadI64(p + 8), p + proto::kHeaderSize, length};
        pos += proto::kHeaderSize + length;
        onPacket(packet);
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + pos);
    parsing_ = false;
}

} // namespace mycam

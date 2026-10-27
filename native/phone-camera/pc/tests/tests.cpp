// Unit tests for the pure parts of the PC side: packet parsing, command encoding (checked against
// protocol/golden.txt), and the NV12 rotate/scale/letterbox used by the virtual camera.
// Run: ctest --test-dir build -C Release   (or build\Release\mycam_tests.exe)

#include <stdio.h>
#include <string.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../companion/packet_parser.h"
#include "../companion/marquee.h"
#include "../companion/protocol.h"
#include "../companion/wifi_crypto.h"
#include "../companion/web_proto.h"
#include "../companion/capabilities.h"
#include "../companion/discovery.h"
#include "../companion/preview_convert.h"
#include "../companion/control_proto.h"
#include "../companion/status_text.h"
#include "../companion/ui_layout.h"
#include "../companion/ui_model.h"
#include "../companion/ui_motion.h"
#include "../vcam/frame_transform.h"
#ifdef _WIN32 // The DirectShow camera and driver binding are Windows-only; everything else here builds everywhere.
#include "../vcam-dshow/dshow_media_types.h"
#include "../companion/winusb_bind.h"
#endif

#include <cmath>

#undef small // rpcndr.h (via status_text.h -> windows.h) defines it; the marquee tests use it as a name.

using namespace mycam;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

using Bytes = std::vector<uint8_t>;

Bytes Hex(std::string s) {
    Bytes out;
    std::string digits;
    for (char c : s) if (isxdigit(static_cast<unsigned char>(c))) digits += c;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(uint8_t(std::stoul(digits.substr(i, 2), nullptr, 16)));
    return out;
}

std::map<std::string, Bytes> LoadGolden() {
    std::map<std::string, Bytes> golden;
    std::ifstream in(MYCAM_GOLDEN_PATH);
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        golden[name] = Hex(line.substr(eq + 1));
    }
    return golden;
}

std::vector<Packet> ParseAll(PacketParser& parser, const Bytes& data, std::vector<Bytes>* payloads = nullptr) {
    std::vector<Packet> packets;
    parser.Feed(data.data(), data.size(), [&](const Packet& p) {
        packets.push_back(p);
        if (payloads) payloads->emplace_back(p.payload, p.payload + p.length);
    });
    return packets;
}

// --- Protocol ---------------------------------------------------------------------------------

void TestGoldenPackets(std::map<std::string, Bytes>& golden) {
    struct Expect { const char* name; uint8_t type; uint8_t flags; int64_t pts; const char* payload; };
    const Expect cases[] = {
        {"packet.hello", proto::kHello, 0, 0, "0001"},
        {"packet.config", proto::kConfig, 0, 0, "0780 0438 005A 01 0000000167"},
        {"packet.frame", proto::kFrame, proto::kFlagKeyFrame, 0x0102030405060708LL, "DEADBEEF"},
        {"packet.orient", proto::kOrient, 0, 0, "010E"},
        {"packet.state", proto::kState, 0, 0, "01 01"},
        {"packet.log", proto::kLog, 0, 0, "6869"},
        {"packet.state_paused", proto::kState, 0, 0, "03 00"},
    };
    for (const Expect& e : cases) {
        CHECK(golden.count(e.name) == 1);
        PacketParser parser;
        std::vector<Bytes> payloads;
        auto packets = ParseAll(parser, golden[e.name], &payloads);
        CHECK(packets.size() == 1);
        if (packets.size() != 1) continue;
        CHECK(packets[0].type == e.type);
        CHECK(packets[0].flags == e.flags);
        CHECK(packets[0].ptsUs == e.pts);
        CHECK(payloads[0] == Hex(e.payload));
        CHECK(parser.Buffered() == 0);
    }
    // Spot-check the constants the golden payloads rely on.
    CHECK(Hex("01 01")[0] == proto::kStateStreaming);
    CHECK(Hex("01 01")[1] == proto::kFacingFront);
    CHECK(Hex("03 00")[0] == proto::kStatePaused);
}

void TestGoldenCommands(std::map<std::string, Bytes>& golden) {
    struct Expect { const char* name; proto::Command cmd; uint8_t arg; };
    const Expect cases[] = {
        {"command.hello", proto::kCmdHello, 0},
        {"command.start", proto::kCmdStart, 0},
        {"command.stop", proto::kCmdStop, 0},
        {"command.keyframe", proto::kCmdKeyFrame, 0},
        {"command.facing_front", proto::kCmdSetFacing, proto::kFacingFront},
        {"command.pause", proto::kCmdPause, 0},
        {"command.resume", proto::kCmdResume, 0},
        {"command.quality_4k", proto::kCmdSetQuality, proto::kQuality4K},
        {"command.fps_60", proto::kCmdSetFps, 60},
        {"command.fps_120", proto::kCmdSetFps, 120},
        {"command.zoom_2x", proto::kCmdSetZoom, 20},
        {"command.exposure_m2", proto::kCmdSetExposure, uint8_t(int8_t(-2))},
        {"command.torch_on", proto::kCmdSetTorch, 1},
        {"command.focus_lock", proto::kCmdSetFocus, 1},
    };
    for (const Expect& e : cases) {
        uint8_t out[proto::kCommandSize];
        proto::MakeCommand(out, e.cmd, e.arg);
        CHECK(Bytes(out, out + sizeof(out)) == golden[e.name]);
    }
}

void TestGoldenCamera(std::map<std::string, Bytes>& golden) {
    PacketParser parser;
    std::vector<Bytes> payloads;
    auto packets = ParseAll(parser, golden["packet.camera"], &payloads);
    CHECK(packets.size() == 1 && packets[0].type == proto::kCamera);
    proto::CameraInfo c;
    CHECK(!payloads.empty() && proto::ParseCameraInfo(payloads[0].data(), uint32_t(payloads[0].size()), &c));
    CHECK(c.quality == proto::kQuality1080p && c.fps == 30);
    CHECK(c.zoomX100 == 100 && c.zoomMinX100 == 60 && c.zoomMaxX100 == 1000);
    CHECK(c.ev == 0 && c.evMin == -12 && c.evMax == 12 && c.evStepX100 == 33);
    CHECK(c.flags == (proto::kCamTorchAvailable | proto::kCamHas60Fps | proto::kCamHas4K));
    CHECK(c.width == 1920 && c.height == 1080 && c.actualFps == 30);
    // Pre-1.3.2 payload: no per-quality masks, so they come from the overall flags (60 yes, 120 no).
    CHECK(!c.hasFpsModes && proto::FpsMask(c, proto::kQuality4K) == (proto::kFps30Bit | proto::kFps60Bit));

    payloads.clear();
    packets = ParseAll(parser, golden["packet.camera_modes"], &payloads);
    proto::CameraInfo m;
    CHECK(packets.size() == 1 && proto::ParseCameraInfo(payloads[0].data(), uint32_t(payloads[0].size()), &m));
    CHECK(m.fps == 120 && m.actualFps == 120 && (m.flags & proto::kCamHas120Fps));
    CHECK(m.hasFpsModes);
    CHECK(proto::FpsMask(m, proto::kQuality720p) == (proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit));
    CHECK(proto::FpsMask(m, proto::kQuality1080p) == (proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit));
    CHECK(proto::FpsMask(m, proto::kQuality4K) == proto::kFps30Bit); // 4K: 30 only, so 60/120 stay disabled.
}

void TestParserSplitsAndBatches(std::map<std::string, Bytes>& golden) {
    Bytes stream = golden["packet.hello"];
    Bytes frame = golden["packet.frame"];
    stream.insert(stream.end(), frame.begin(), frame.end());

    // One byte at a time.
    PacketParser parser;
    int count = 0;
    for (uint8_t b : stream) parser.Feed(&b, 1, [&](const Packet&) { ++count; });
    CHECK(count == 2);
    CHECK(parser.Buffered() == 0);

    // Both packets in a single read.
    PacketParser batch;
    CHECK(ParseAll(batch, stream).size() == 2);
}

void TestParserResyncs(std::map<std::string, Bytes>& golden) {
    // Garbage, a fake magic, and an absurd length must all be skipped.
    Bytes data = Hex("00 11 22 4D43414D 4D43414D 02 00 0000 0000000000000000 FFFFFFFF");
    Bytes hello = golden["packet.hello"];
    data.insert(data.end(), hello.begin(), hello.end());
    PacketParser parser;
    auto packets = ParseAll(parser, data);
    CHECK(packets.size() == 1);
    CHECK(!packets.empty() && packets[0].type == proto::kHello);
}

void TestParserReentrant(std::map<std::string, Bytes>& golden) {
    // A callback that feeds more data (as a nested USB event loop can) must not break parsing.
    Bytes hello = golden["packet.hello"];
    Bytes frame = golden["packet.frame"];
    PacketParser parser;
    std::vector<uint8_t> types;
    bool fed = false;
    std::function<void(const Packet&)> onPacket = [&](const Packet& p) {
        types.push_back(p.type);
        if (!fed) {
            fed = true;
            parser.Feed(frame.data(), frame.size(), onPacket);
        }
    };
    parser.Feed(hello.data(), hello.size(), onPacket);
    CHECK(types.size() == 2);
    CHECK(types.size() == 2 && types[0] == proto::kHello && types[1] == proto::kFrame);
    CHECK(parser.Buffered() == 0);
}

// --- Wireless crypto (golden.txt "wifi.*", made with the JDK) -----------------------------------

void TestWifiCrypto(std::map<std::string, Bytes>& golden) {
    using namespace mycam::wifi;
    CHECK(Hkdf(Hex(std::string(44, 'B').replace(0, 44, "0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B")), Hex("000102030405060708090A0B0C"),
               Hex("F0F1F2F3F4F5F6F7F8F9"), 42) == golden["wifi.hkdf_rfc5869_1"]);

    // ECDH both ways; also proves the CNG raw secret is byte-reversed into Java's order.
    const Bytes pcPub = golden["wifi.pc_public"], phonePub = golden["wifi.phone_public"];
    EcKey pc, phone;
    CHECK(pc.ImportPrivate(golden["wifi.pc_private"], pcPub));
    CHECK(phone.ImportPrivate(golden["wifi.phone_private"], phonePub));
    CHECK(pc.PublicRaw() == pcPub);
    Bytes z1, z2;
    CHECK(pc.Agree(phonePub, &z1) && z1 == golden["wifi.ecdh"]);
    CHECK(phone.Agree(pcPub, &z2) && z2 == golden["wifi.ecdh"]);
    Bytes bad = phonePub;
    bad[40] ^= 1; // Not on the curve any more: must be refused.
    CHECK(!pc.Agree(bad, &z1));

    const Bytes z = golden["wifi.ecdh"], na = golden["wifi.na"], nb = golden["wifi.nb"];
    CHECK(Commit(nb, phonePub, pcPub) == golden["wifi.commit"]);
    CHECK(Sas(pcPub, phonePub, na, nb) == "554294"); // golden "wifi.sas" holds these digits as text.
    const Bytes pairKey = PairKey(z, na, nb);
    CHECK(pairKey == golden["wifi.pair_key"]);
    const Bytes okm = SessionKeys(z, pairKey, golden["wifi.npc"], golden["wifi.nph"]);
    CHECK(okm == golden["wifi.session_okm"]);
    const Bytes kFin(okm.begin() + 64, okm.end());
    CHECK(Finished(kFin, "PC", golden["wifi.transcript"]) == golden["wifi.finished_pc"]);
    CHECK(Finished(kFin, "PH", golden["wifi.transcript"]) == golden["wifi.finished_phone"]);

    // Records: exact bytes, counters, tamper and direction checks.
    const Bytes k1(okm.begin(), okm.begin() + 32);
    RecordKey tx, rx;
    CHECK(tx.Init(k1, kDirPcToPhone));
    Bytes record;
    const char hello[] = "MCMD hello";
    CHECK(tx.Seal(reinterpret_cast<const uint8_t*>(hello), strlen(hello), &record));
    CHECK(record == golden["wifi.record_pc_0"]);

    RecordKey phoneTx;
    CHECK(phoneTx.Init(k1, kDirPhoneToPc));
    Bytes empty;
    for (int i = 0; i < 7; ++i) { Bytes skip; phoneTx.Seal(nullptr, 0, &skip); }
    CHECK(phoneTx.Seal(nullptr, 0, &empty) && empty == golden["wifi.record_phone_7"]);

    CHECK(rx.Init(k1, kDirPcToPhone));
    Bytes plain;
    CHECK(rx.Open(record.data() + 4, record.size() - 4, &plain) && Bytes(plain) == Bytes(hello, hello + strlen(hello)));
    CHECK(!rx.Open(record.data() + 4, record.size() - 4, &plain)); // Replayed: counter has moved on.
    RecordKey rx2;
    rx2.Init(k1, kDirPcToPhone);
    Bytes tampered = record;
    tampered[8] ^= 1;
    CHECK(!rx2.Open(tampered.data() + 4, tampered.size() - 4, &plain));
}

// --- Frame transform -------------------------------------------------------------------------

// A w x h NV12 image whose luma is a unique value per pixel (y * w + x + 1) and whose chroma pairs
// encode their UV coordinates, so every sampled pixel can be traced back.
Bytes MakeImage(int w, int h) {
    Bytes img(size_t(w) * h * 3 / 2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img[y * w + x] = uint8_t(y * w + x + 1);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            img[w * h + y * w + x * 2] = uint8_t(100 + x);
            img[w * h + y * w + x * 2 + 1] = uint8_t(200 + y);
        }
    return img;
}

Bytes Render(const Bytes& src, int w, int h, uint32_t rot, bool mirror, int W, int H) {
    Bytes dst(size_t(W) * H * 3 / 2);
    FillBlackNV12(dst.data(), W, W, H);
    DrawFittedNV12(src.data(), w, h, rot, mirror, dst.data(), W, W, H);
    return dst;
}

void TestIdentity() {
    Bytes src = MakeImage(4, 4);
    CHECK(Render(src, 4, 4, 0, false, 4, 4) == src);
}

void TestRotations() {
    // 4x2 source:   1 2 3 4
    //               5 6 7 8
    Bytes src = MakeImage(4, 2);
    // 90 clockwise -> 2x4:  5 1 / 6 2 / 7 3 / 8 4
    Bytes r90 = Render(src, 4, 2, 90, false, 2, 4);
    CHECK((Bytes(r90.begin(), r90.begin() + 8) == Bytes{5, 1, 6, 2, 7, 3, 8, 4}));
    // 180 -> 8 7 6 5 / 4 3 2 1
    Bytes r180 = Render(src, 4, 2, 180, false, 4, 2);
    CHECK((Bytes(r180.begin(), r180.begin() + 8) == Bytes{8, 7, 6, 5, 4, 3, 2, 1}));
    // 270 clockwise -> 2x4:  4 8 / 3 7 / 2 6 / 1 5
    Bytes r270 = Render(src, 4, 2, 270, false, 2, 4);
    CHECK((Bytes(r270.begin(), r270.begin() + 8) == Bytes{4, 8, 3, 7, 2, 6, 1, 5}));
}

void TestMirror() {
    Bytes src = MakeImage(4, 2);
    Bytes m = Render(src, 4, 2, 0, true, 4, 2);
    CHECK((Bytes(m.begin(), m.begin() + 8) == Bytes{4, 3, 2, 1, 8, 7, 6, 5}));
    // Chroma mirrors too: UV columns 0,1 swap.
    CHECK(m[8] == 101 && m[10] == 100);
}

void TestLetterbox() {
    // A 2x4 (portrait) image into an 8x4 output: scaled 1:1, centred at x=3 -> even-aligned to x=2.
    Bytes src = MakeImage(2, 4);
    Bytes out = Render(src, 2, 4, 0, false, 8, 4);
    for (int y = 0; y < 4; ++y) {
        CHECK(out[y * 8 + 0] == 16 && out[y * 8 + 1] == 16);           // Left bar is black.
        CHECK(out[y * 8 + 2] == src[y * 2] && out[y * 8 + 3] == src[y * 2 + 1]); // Image.
        CHECK(out[y * 8 + 4] == 16 && out[y * 8 + 7] == 16);           // Right bar is black.
    }
    CHECK(out[8 * 4 + 0] == 128 && out[8 * 4 + 1] == 128); // Black chroma in the bar.
}

void TestFill() {
    // A 2x4 portrait image filling an 8x4 output: scaled 4x to 8x16, centred, so rows 6..9 of the scaled
    // image are visible = source rows 1..2. No black bars anywhere.
    Bytes src = MakeImage(2, 4); // luma: 1 2 / 3 4 / 5 6 / 7 8
    Bytes dst(8 * 4 * 3 / 2);
    FillBlackNV12(dst.data(), 8, 8, 4);
    DrawFittedNV12(src.data(), 2, 4, 0, false, dst.data(), 8, 8, 4, true);
    CHECK((Bytes(dst.begin(), dst.begin() + 8) == Bytes{3, 3, 3, 3, 4, 4, 4, 4}));
    CHECK((Bytes(dst.begin() + 24, dst.begin() + 32) == Bytes{5, 5, 5, 5, 6, 6, 6, 6}));
    bool anyBlack = false;
    for (size_t i = 0; i < 32; ++i) anyBlack |= dst[i] == 16;
    for (size_t i = 32; i < dst.size(); ++i) anyBlack |= dst[i] == 128;
    CHECK(!anyBlack);
    // Fill of a same-aspect image is identical to fit.
    Bytes sq = MakeImage(4, 4);
    CHECK(Render(sq, 4, 4, 0, false, 4, 4) == [&] {
        Bytes d(4 * 4 * 3 / 2);
        FillBlackNV12(d.data(), 4, 4, 4);
        DrawFittedNV12(sq.data(), 4, 4, 0, false, d.data(), 4, 4, 4, true);
        return d;
    }());
}

void TestDownscale() {
    // 8x8 -> 4x4 picks every other pixel.
    Bytes src = MakeImage(8, 8);
    Bytes out = Render(src, 8, 8, 0, false, 4, 4);
    CHECK(out[0] == src[0] && out[1] == src[2] && out[4] == src[16] && out[5] == src[18]);
}

void TestPitch() {
    // Destination rows wider than the image (padding) must not be written past the image width.
    const int W = 4, H = 2, pitch = 8;
    Bytes src = MakeImage(4, 2);
    Bytes dst(size_t(pitch) * H * 3 / 2, 0xAB);
    FillBlackNV12(dst.data(), pitch, W, H);
    DrawFittedNV12(src.data(), 4, 2, 0, false, dst.data(), pitch, W, H);
    CHECK(dst[0] == 1 && dst[3] == 4 && dst[pitch] == 5);
    CHECK(dst[4] == 0xAB && dst[pitch + 7] == 0xAB); // Padding untouched.
}

void TestBgraToNV12() {
    // 2x2 blocks of solid colours -> known BT.601 limited-range values.
    struct Case { uint8_t r, g, b, y, u, v; };
    const Case cases[] = {
        {255, 255, 255, 235, 128, 128}, // white
        {0, 0, 0, 16, 128, 128},        // black
        {255, 0, 0, 82, 90, 240},       // red
        {128, 128, 128, 126, 128, 128}, // mid gray
    };
    for (const Case& c : cases) {
        uint8_t bgra[16];
        for (int i = 0; i < 4; ++i) { bgra[i * 4] = c.b; bgra[i * 4 + 1] = c.g; bgra[i * 4 + 2] = c.r; bgra[i * 4 + 3] = 255; }
        uint8_t out[6] = {};
        BgraToNV12(bgra, 8, 2, 2, out);
        CHECK(out[0] == c.y && out[1] == c.y && out[2] == c.y && out[3] == c.y);
        CHECK(out[4] == c.u && out[5] == c.v);
    }
    // Chroma averages a 2x2 block: half black, half white -> neutral chroma.
    uint8_t mixed[16] = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    uint8_t out[6] = {};
    BgraToNV12(mixed, 8, 2, 2, out);
    CHECK(out[0] == 16 && out[1] == 235 && out[4] == 128 && out[5] == 128);
}

void TestNv12ToI420() {
    // 4x2: Y bytes 0..7, then two interleaved UV pairs.
    const uint8_t nv12[12] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 20, 11, 21};
    uint8_t out[12] = {};
    Nv12ToI420(nv12, 4, 2, out);
    const uint8_t expect[12] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 20, 21};
    CHECK(memcmp(out, expect, sizeof(out)) == 0);
}

#ifdef _WIN32
// --- pc/vcam-dshow: AM_MEDIA_TYPE helpers (format negotiation, no DirectShow graph needed) ----------

void TestDShowMediaTypes() {
    for (const auto& format : kDShowFormats) {
        AM_MEDIA_TYPE* mt = NewMediaType(format);
        CHECK(mt != nullptr);
        CHECK(mt->majortype == MEDIATYPE_Video);
        CHECK(mt->subtype == kSubtypeNV12);
        CHECK(mt->formattype == FORMAT_VideoInfo);
        CHECK(mt->bFixedSizeSamples == TRUE && mt->bTemporalCompression == FALSE);
        CHECK(mt->lSampleSize == LONG(FrameBytes(format.width, format.height)));
        CHECK(mt->cbFormat == sizeof(VIDEOINFOHEADER) && mt->pbFormat != nullptr);
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
        CHECK(UINT32(info->bmiHeader.biWidth) == format.width);
        CHECK(UINT32(info->bmiHeader.biHeight) == format.height);
        CHECK(info->AvgTimePerFrame == 10'000'000LL / format.fps);

        DShowFormat matched{};
        CHECK(MatchesKnownFormat(*mt, &matched));
        CHECK(matched.width == format.width && matched.height == format.height && matched.fps == format.fps);

        VIDEO_STREAM_CONFIG_CAPS caps{};
        FillStreamCaps(format, &caps);
        CHECK(caps.InputSize.cx == LONG(format.width) && caps.InputSize.cy == LONG(format.height));
        CHECK(caps.MinFrameInterval == caps.MaxFrameInterval && caps.MinFrameInterval == 10'000'000LL / format.fps);

        AM_MEDIA_TYPE copy{};
        CHECK(CopyMediaTypeInto(*mt, &copy));
        CHECK(copy.pbFormat != mt->pbFormat); // Deep copy: distinct allocations.
        CHECK(memcmp(copy.pbFormat, mt->pbFormat, copy.cbFormat) == 0);
        FreeMediaType(&copy);
        CHECK(copy.pbFormat == nullptr && copy.cbFormat == 0);

        FreeMediaType(mt);
        CHECK(mt->pbFormat == nullptr);
        CoTaskMemFree(mt);
    }

    // A resolution/fps combination not in the list (or the wrong subtype) is rejected.
    AM_MEDIA_TYPE* mt = NewMediaType(kDShowFormats[0]);
    auto* info = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
    info->bmiHeader.biWidth = 1234;
    DShowFormat ignored{};
    CHECK(!MatchesKnownFormat(*mt, &ignored));
    mt->subtype.Data1 ^= 1; // Any other subtype: no named GUID constant needed for this check.
    info->bmiHeader.biWidth = LONG(kDShowFormats[0].width);
    CHECK(!MatchesKnownFormat(*mt, &ignored));
    FreeMediaType(mt);
    CoTaskMemFree(mt);
}

// --- pc/companion/winusb_bind: which device instances get WinUSB (#9) ------------------------------

void TestAccessoryInstances() {
    CHECK(IsAccessoryInstance(L"USB\\VID_18D1&PID_2D00\\0123456789ABCDEF"));        // Accessory only.
    CHECK(IsAccessoryInstance(L"usb\\vid_18d1&pid_2d00\\0123"));                    // Case doesn't matter.
    CHECK(IsAccessoryInstance(L"USB\\VID_18D1&PID_2D01&MI_00\\6&1A2B3C4D&0&0000"));  // Accessory + adb (USB debugging).
    CHECK(!IsAccessoryInstance(L"USB\\VID_18D1&PID_2D01&MI_01\\6&1A2B3C4D&0&0001")); // ...and its adb interface.
    CHECK(IsAccessoryInstance(L"USB\\VID_18D1&PID_2D04&MI_00\\6&1&0&0000"));         // Accessory + audio.
    CHECK(IsAccessoryInstance(L"USB\\VID_18D1&PID_2D05&MI_00\\6&1&0&0000"));         // Accessory + audio + adb.
    CHECK(!IsAccessoryInstance(L"USB\\VID_18D1&PID_2D05&MI_03\\6&1&0&0003"));
    CHECK(!IsAccessoryInstance(L"USB\\VID_18D1&PID_2D03&MI_00\\6&1&0&0000"));        // Audio only: MI_00 is audio.
    CHECK(!IsAccessoryInstance(L"USB\\VID_18D1&PID_2D06\\0123"));                    // Not an accessory product.
    CHECK(!IsAccessoryInstance(L"USB\\VID_18D1&PID_4EE7\\0123"));                    // A Pixel in normal (MTP) mode.
    CHECK(!IsAccessoryInstance(L"USB\\VID_18D1&PID_2D0"));                           // Truncated.
}
#endif

// --- Waiting-picture marquee ----------------------------------------------------------------------

Nv12Image PatternImage(uint32_t w, uint32_t h) {
    Nv12Image img;
    img.width = w;
    img.height = h;
    img.data.resize(size_t(w) * h * 3 / 2);
    for (size_t i = 0; i < img.data.size(); ++i) img.data[i] = uint8_t((i * 7 + i / w * 13) & 0xFF);
    return img;
}

// True if pixel (x, y) is covered by a visible chunk at time t (clipped to the interior).
bool InChunk(const MarqueeGeometry& g, double t, int x, int y) {
    if (y < g.iy || y >= g.iy + g.ih || x < g.ix || x >= g.ix + g.iw) return false;
    for (int i = 0; i < kMarqueeChunks; ++i) {
        int left = 0;
        if (MarqueeChunkLeft(g, t, i, &left) && x >= left && x < left + g.chunkW) return true;
    }
    return false;
}

void TestMarqueeGeometry() {
    MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    CHECK(g.valid());
    CHECK(g.x == kMarqueeTrackX && g.y == kMarqueeTrackY && g.w == kMarqueeTrackW && g.h == kMarqueeTrackH);
    CHECK(g.ix == g.x + 2 && g.iy == g.y + 2 && g.iw == g.w - 4 && g.ih == g.h - 4);
    CHECK(g.chunkW == 8 && g.gap == 2);
    MarqueeGeometry half = MarqueeGeometryFor(640, 360);
    CHECK(half.valid() && half.chunkW == 4 && half.gap == 1);
    CHECK(half.x == (kMarqueeTrackX + 1) / 2 && half.w == kMarqueeTrackW / 2);
    MarqueeGeometry big = MarqueeGeometryFor(1920, 1080);
    CHECK(big.chunkW == 12 && big.gap == 3 && big.x == kMarqueeTrackX * 3 / 2);
    CHECK(!MarqueeGeometryFor(0, 0).valid());
    CHECK(!MarqueeGeometryFor(16, 16).valid()); // Track falls outside a tiny image.
}

void TestMarqueeChunkPositions() {
    const MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    const int group = 3 * g.chunkW + 2 * g.gap; // 28
    int l0 = 0, l1 = 0, l2 = 0;
    // Start of a pass: the whole group sits just left of the interior.
    CHECK(MarqueeChunkLeft(g, 0.0, 0, &l0) && MarqueeChunkLeft(g, 0.0, 2, &l2));
    CHECK(l2 == g.ix - group && l0 + g.chunkW == g.ix);
    // Halfway: linear, chunk 0 leads, chunks 10 px apart.
    CHECK(MarqueeChunkLeft(g, 1.0, 0, &l0) && MarqueeChunkLeft(g, 1.0, 1, &l1) && MarqueeChunkLeft(g, 1.0, 2, &l2));
    CHECK(l2 == g.ix - group + (g.iw + group) / 2);
    CHECK(l1 == l2 + g.chunkW + g.gap && l0 == l1 + g.chunkW + g.gap);
    // Monotonic left to right over the pass, and the group ends past the right edge.
    int prev = -100000;
    bool monotonic = true;
    for (double t = 0; t < kMarqueePassSec; t += 0.01) {
        int l = 0;
        CHECK(MarqueeChunkLeft(g, t, 2, &l));
        if (l < prev) monotonic = false;
        prev = l;
    }
    CHECK(monotonic);
    CHECK(MarqueeChunkLeft(g, 1.9999, 2, &l2) && l2 >= g.ix + g.iw - 1);
    // Repeats every pass + pause.
    const double period = kMarqueePassSec + kMarqueePauseSec;
    CHECK(MarqueeChunkLeft(g, 1.0 + 5 * period, 0, &l1) && MarqueeChunkLeft(g, 1.0, 0, &l0) && l0 == l1);
    CHECK(!MarqueeChunkLeft(g, 1.0, 3, &l0)); // Only 3 chunks.
}

void TestMarqueePause() {
    const MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    int l = 0;
    CHECK(!MarqueeChunkLeft(g, 2.0, 0, &l));
    CHECK(!MarqueeChunkLeft(g, 2.2, 1, &l));
    CHECK(!MarqueeChunkLeft(g, 2.39, 2, &l));
    CHECK(MarqueeChunkLeft(g, 2.4, 0, &l));
    const Nv12Image base = PatternImage(1280, 720);
    Nv12Image img = base;
    DrawMarquee(img, base, 1.0);
    CHECK(img.data != base.data);
    DrawMarquee(img, base, 2.2); // Pause: the empty track (the base picture) comes back.
    CHECK(img.data == base.data);
}

void TestMarqueeDraw(uint32_t w, uint32_t h) {
    const MarqueeGeometry g = MarqueeGeometryFor(w, h);
    const Nv12Image base = PatternImage(w, h);
    const int x0 = g.x & ~1, y0 = g.y & ~1, x1 = (g.x + g.w + 1) & ~1, y1 = (g.y + g.h + 1) & ~1;
    const uint8_t* by = base.data.data();
    const uint8_t* buv = by + size_t(w) * h;
    for (double t : {0.05, 0.37, 1.0, 1.23, 1.71, 1.98}) {
        Nv12Image img = base;
        DrawMarquee(img, base, t);
        const uint8_t* y = img.data.data();
        const uint8_t* uv = y + size_t(w) * h;
        bool outsideClean = true, chunksDrawn = true, gapsClean = true, chromaOk = true;
        int painted = 0;
        for (uint32_t r = 0; r < h; ++r) {
            for (uint32_t c = 0; c < w; ++c) {
                const size_t i = size_t(r) * w + c;
                const bool inRect = int(c) >= x0 && int(c) < x1 && int(r) >= y0 && int(r) < y1;
                if (!inRect && y[i] != by[i]) outsideClean = false;
                if (InChunk(g, t, c, r)) ++painted;
                else if (inRect && y[i] != by[i]) gapsClean = false;
            }
        }
        // Chroma: changed only for whole blocks inside a chunk; every such block is painted green.
        for (uint32_t br = 0; br < h / 2; ++br) {
            for (uint32_t bx = 0; bx < w / 2; ++bx) {
                const size_t i = size_t(br) * w + 2 * bx;
                const bool full = InChunk(g, t, 2 * bx, 2 * br) && InChunk(g, t, 2 * bx + 1, 2 * br) &&
                                  InChunk(g, t, 2 * bx, 2 * br + 1) && InChunk(g, t, 2 * bx + 1, 2 * br + 1);
                const bool changed = uv[i] != buv[i] || uv[i + 1] != buv[i + 1];
                const bool inRect = int(2 * bx) >= x0 && int(2 * bx) < x1 && int(2 * br) >= y0 && int(2 * br) < y1;
                if (!inRect && changed) outsideClean = false;
                if (!full && inRect && changed) chromaOk = false;
                if (full && !(uv[i] < 128 && uv[i + 1] < 128)) chromaOk = false; // Green: U and V below neutral.
            }
        }
        // Painted pixels: lighter at the top highlight than at the dark 55 % band.
        for (int c = g.ix; c < g.ix + g.iw; ++c) {
            if (!InChunk(g, t, c, g.iy)) continue;
            if (!(y[size_t(g.iy) * w + c] > y[size_t(g.iy + g.ih * 55 / 100) * w + c])) chunksDrawn = false;
        }
        CHECK(outsideClean);
        CHECK(gapsClean);
        CHECK(chromaOk);
        CHECK(chunksDrawn);
        CHECK(painted > 0);
    }
}

void TestMarqueeRestore() {
    const Nv12Image base = PatternImage(1280, 720);
    Nv12Image img = base;
    // Successive frames only ever show the current chunks: earlier ones are wiped by the restore.
    DrawMarquee(img, base, 0.5);
    DrawMarquee(img, base, 1.5);
    Nv12Image fresh = base;
    DrawMarquee(fresh, base, 1.5);
    CHECK(img.data == fresh.data);
    // Garbage inside the track is restored too.
    const MarqueeGeometry g = MarqueeGeometryFor(1280, 720);
    for (int r = g.y; r < g.y + g.h; ++r) memset(img.data.data() + size_t(r) * 1280 + g.x, 0x55, g.w);
    DrawMarquee(img, base, 2.3);
    CHECK(img.data == base.data);
    // Mismatched sizes are ignored rather than overrun.
    Nv12Image small = PatternImage(640, 360);
    const Nv12Image before = small;
    DrawMarquee(small, base, 1.0);
    CHECK(small.data == before.data);
}

// --- Companion UI (layout, motion, capabilities, element model, preview, status text) ---------------

void TestUiFlow() {
    using namespace mycam::ui;
    float bottom = 0;
    // Three 60-wide items in 150: two on the first line, the third wraps.
    auto boxes = Flow({{60, 16}, {60, 16}, {60, 16}}, 10, 20, 150, 10, 4, &bottom);
    CHECK(boxes.size() == 3);
    CHECK(boxes[0].l == 10 && boxes[1].l == 80 && boxes[0].t == 20);
    CHECK(boxes[2].l == 10 && boxes[2].t == 40);
    CHECK(bottom == 56);
    for (size_t i = 0; i < boxes.size(); ++i) {
        CHECK(boxes[i].r <= 160.01f);
        for (size_t j = i + 1; j < boxes.size(); ++j) CHECK(!boxes[i].Intersects(boxes[j]));
    }
    // An item wider than the line gets its own line, clipped to the width; shorter items are centred.
    boxes = Flow({{300, 20}, {40, 10}, {40, 20}}, 0, 0, 100, 6, 0, &bottom);
    CHECK(boxes[0].W() == 100 && boxes[1].t == 20 + 5 && boxes[2].t == 20);
    CHECK(Flow({}, 0, 7, 100, 0, 0, &bottom).empty() && bottom == 7);
    // Split: fixed columns first, the flexible ones share the rest.
    auto cols = Split(0, 0, 100, 10, {20, 0, 0}, 10);
    CHECK(cols[0].W() == 20 && cols[1].W() == 30 && cols[2].l == 70 && cols[2].r == 100);
    Column c(5, 5, 50);
    Box a = c.Row(10);
    c.Gap(3);
    Box b = c.Item({20, 8}, 2);
    CHECK(a.t == 5 && b.t == 18 && b.l == 35 && c.Y() == 26);
}

void TestUiMotion() {
    using namespace mycam::ui;
    CHECK(EaseOut(0) == 0 && EaseOut(1) == 1 && EaseInOut(0.5f) > 0.49f && EaseInOut(0.5f) < 0.51f);
    CHECK(std::fabs(BackOut(1) - 1) < 1e-5f);
    bool overshoots = false;
    for (int i = 1; i < 20; ++i) overshoots |= BackOut(i / 20.f) > 1.f;
    CHECK(overshoots);
    Tween t;
    t.Start(1, 1000, 200, 0);
    CHECK(t.Running(1100) && !t.Running(1200) && t.Value(1000) == 0 && t.Value(1300) == 1);
    t.Start(0, 1000, 0, 1); // Reduced motion: instant.
    CHECK(!t.Running(1000) && t.Value(1000) == 0);
    // Marquee: snaps to chunk steps, crosses the whole track in 2.0 s, nothing during the 0.4 s pause.
    const float track = 200, chunk = 8, gap = 2;
    CHECK(MarqueeOffset(0, track, chunk, gap) == -30);
    for (uint64_t ms = 0; ms < 2000; ms += 37) {
        const float x = MarqueeOffset(ms, track, chunk, gap);
        CHECK(std::fmod(x, 10.f) == 0 && x >= -30 && x <= track);
    }
    CHECK(MarqueeOffset(2100, track, chunk, gap) >= track);
    CHECK(MarqueeOffset(2400, track, chunk, gap) == -30); // Next pass.
    CHECK(MarqueeOffset(1000, track, chunk, gap) > MarqueeOffset(500, track, chunk, gap));
    // Determinate: whole chunks only.
    CHECK(DeterminateChunks(1, 98, 8, 2) == 10 && DeterminateChunks(0, 98, 8, 2) == 0);
    CHECK(DeterminateChunks(0.55f, 98, 8, 2) == 5 && DeterminateChunks(2, 98, 8, 2) == 10);
    wchar_t text[8];
    FormatCountdown(42000, text, 8);
    CHECK(wcscmp(text, L"0:42") == 0);
    FormatCountdown(60000, text, 8);
    CHECK(wcscmp(text, L"1:00") == 0);
    FormatCountdown(1, text, 8);
    CHECK(wcscmp(text, L"0:01") == 0);
}

void TestCapabilities() {
    proto::CameraInfo c;
    CHECK(!caps::QualityEnabled(true, c, proto::kQuality720p)); // No CameraInfo yet.
    c.valid = true;
    CHECK(caps::QualityEnabled(true, c, proto::kQuality4K));   // Unknown until the camera starts: offered.
    CHECK(caps::FpsEnabled(true, c, 30) && !caps::FpsEnabled(true, c, 60));
    c.width = 1920;
    c.height = 1080;
    c.quality = proto::kQuality1080p;
    c.hasFpsModes = true;
    c.fpsModes[0] = proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit;
    c.fpsModes[1] = proto::kFps30Bit | proto::kFps60Bit;
    c.fpsModes[2] = proto::kFps30Bit;
    CHECK(!caps::QualityEnabled(true, c, proto::kQuality4K)); // Known, and no 4K flag.
    CHECK(caps::FpsEnabled(true, c, 60) && !caps::FpsEnabled(true, c, 120));
    c.fps = 120;
    CHECK(caps::EffectiveFps(c) == 60); // 120 asked, 60 is the best this quality does.
    c.zoomX100 = 100; c.zoomMinX100 = 60; c.zoomMaxX100 = 500;
    CHECK(caps::ZoomOutEnabled(true, c) && caps::ZoomInEnabled(true, c) && !caps::ZoomResetEnabled(true, c));
    CHECK(!caps::AutoEnabled(true, c));
    c.ev = 1;
    CHECK(caps::AutoEnabled(true, c) && !caps::EvUpEnabled(true, c)); // evMax == evMin: no exposure control.
    CHECK(!caps::QualityEnabled(false, c, proto::kQuality720p));
    CHECK(wcscmp(caps::QualityLabel(1080), L"1080p") == 0 && wcscmp(caps::QualityLabel(2160), L"4K") == 0);
}

void TestUiModel() {
    using namespace mycam::ui;
    auto make = [](int id, Kind kind, int parent, int set = kSetNone, bool selected = false, bool enabled = true) {
        Element e;
        e.id = id;
        e.kind = kind;
        e.parent = parent;
        e.radioSet = set;
        e.selected = selected;
        e.enabled = enabled;
        e.focusable = kind != Kind::Text;
        return e;
    };
    std::vector<Element> list = {
        make(kPause, Kind::Button, kGroupNow),
        make(kStatusHeadline, Kind::Text, kGroupNow),
        make(kBack, Kind::Radio, kGroupCamera, kSetFacing, false),
        make(kFront, Kind::Radio, kGroupCamera, kSetFacing, true),
        make(kQ720, Kind::Radio, kGroupVideo, kSetQuality, false),
        make(kQ1080, Kind::Radio, kGroupVideo, kSetQuality, false),
        make(kQ4K, Kind::Radio, kGroupVideo, kSetQuality, false, false),
        make(kMirror, Kind::Checkbox, kGroupPicture),
        make(kFill, Kind::Checkbox, kGroupPicture),
    };
    list[7].name = L"Mirror the image";
    list[7].accessKeyIndex = 0;
    // A radio set is one tab stop: its selected option, else its first enabled one.
    const std::vector<int> stops = TabStops(list);
    CHECK((stops == std::vector<int>{kPause, kFront, kQ720, kMirror, kFill}));
    CHECK(NextTabStop(list, kPause, false) == kFront);
    CHECK(NextTabStop(list, kBack, false) == kQ720);   // From either option of the set.
    CHECK(NextTabStop(list, kFill, false) == kPause);  // Wraps.
    CHECK(NextTabStop(list, kPause, true) == kFill);
    CHECK(NextTabStop(list, kNone, false) == kPause);
    // Arrows stay inside the set and skip disabled options.
    CHECK(ArrowTarget(list, kQ1080, 1) == kQ720);
    CHECK(ArrowTarget(list, kQ720, -1) == kQ1080);
    CHECK(ArrowTarget(list, kFront, 1) == kBack);
    CHECK(ArrowTarget(list, kMirror, 1) == kFill && ArrowTarget(list, kFill, 1) == kMirror);
    // Hidden (collapsed) elements are skipped.
    list[7].visible = false;
    CHECK(NextTabStop(list, kQ720, false) == kFill);
    CHECK(AccessKeyTarget(list, L'm') == kNone);
    list[7].visible = true;
    CHECK(AccessKeyTarget(list, L'M') == kMirror && AccessKeyTarget(list, L'x') == kNone);
    int key = -2;
    CHECK(StripAccessKey(L"&Pause the camera", &key) == L"Pause the camera" && key == 0);
    CHECK(StripAccessKey(L"Fish && chips", &key) == L"Fish & chips" && key == -1);
    Element off = make(kOpenLog, Kind::Link, kGroupTasks);
    off.rect = {0, 600, 50, 615};
    off.clip = {0, 30, 240, 497};
    CHECK(off.Offscreen());
    off.rect = {0, 400, 50, 415};
    CHECK(!off.Offscreen());
}

void TestDiscovery() {
    // Parsing the phone's answer: "MYCAM!1 <tcp port> <phone name>".
    uint16_t port = 0;
    std::string name = "x";
    CHECK(ParseDiscoveryReply("MYCAM!1 47800 Pixel 8", &port, &name) && port == 47800 && name == "Pixel 8");
    CHECK(ParseDiscoveryReply("MYCAM!1 47800", &port, &name) && port == 47800 && name.empty());
    CHECK(ParseDiscoveryReply("MYCAM!1 1 a  b \r\n", &port, &name) && port == 1 && name == "a  b");
    CHECK(!ParseDiscoveryReply("MYCAM?1 DESKTOP", &port, &name));   // Another PC's probe.
    CHECK(!ParseDiscoveryReply("MYCAM!2 47800 x", &port, &name));   // Another version.
    CHECK(!ParseDiscoveryReply("MYCAM!10 47800 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 0 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 65536 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 47a00 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM!1 x", &port, &name));
    CHECK(ParseDiscoveryReply("MYCAM!1 65535 " + std::string(100, 'n'), &port, &name) && port == 65535 && name.size() == 64);

    // "Connect to me": the same body under its own tag, and never confused with an answer.
    CHECK(ParseDiscoveryConnectRequest("MYCAM+1 47800 Pixel 8", &port, &name) && port == 47800 && name == "Pixel 8");
    CHECK(ParseDiscoveryConnectRequest("MYCAM+1 47800", &port, &name) && port == 47800 && name.empty());
    CHECK(!ParseDiscoveryConnectRequest("MYCAM!1 47800 x", &port, &name));
    CHECK(!ParseDiscoveryConnectRequest("MYCAM+2 47800 x", &port, &name));
    CHECK(!ParseDiscoveryConnectRequest("MYCAM+1 0 x", &port, &name));
    CHECK(!ParseDiscoveryReply("MYCAM+1 47800 x", &port, &name));

    // The list: one entry per address, in first-answer order, refreshed by later answers.
    NearbyList list;
    CHECK(list.Seen(1, "10.0.0.1", 47800, "Pixel", 1000));
    CHECK(list.Seen(2, "10.0.0.2", 47800, "", 1100));                   // No name: the address stands in.
    CHECK(!list.Seen(1, "10.0.0.1", 47800, "Pixel", 3000));             // Same answer again: no change.
    CHECK(list.Phones().size() == 2 && list.Phones()[0].lastSeen == 3000 && list.Phones()[1].name == "10.0.0.2");
    CHECK(list.Seen(1, "10.0.0.1", 47801, "Pixel", 3100));              // A new port is a change.
    CHECK(list.Find(1)->port == 47801 && !list.Find(3));
    // Expiry drops the phones that stopped answering, except the connected one.
    CHECK(list.SetConnected(2) && !list.SetConnected(2));
    CHECK(!list.Expire(7000, 6000));
    CHECK(list.Expire(9200, 6000) && list.Phones().size() == 1 && list.Phones()[0].ip == 2);
    CHECK(list.SetConnected(0) && !list.Phones()[0].connected);
    // Paired marks come from a name check.
    list.Seen(5, "10.0.0.5", 47800, "Moto", 9300);
    CHECK(list.UpdatePaired([](const std::string& n) { return n == "Moto"; }) && list.Find(5)->paired && !list.Find(2)->paired);
    CHECK(!list.UpdatePaired([](const std::string& n) { return n == "Moto"; }));
    // A new scan keeps only the connected phone.
    list.SetConnected(5);
    list.ClearForScan();
    CHECK(list.Phones().size() == 1 && list.Phones()[0].ip == 5);
    // Bounded: a 17th phone replaces the one heard from longest ago (never the connected one).
    list.Clear();
    for (uint32_t i = 0; i < NearbyList::kMax; ++i) list.Seen(100 + i, "ip", 1, "p", 10000 + i);
    list.SetConnected(100);
    CHECK(list.Seen(200, "ip", 1, "new", 20000) && list.Phones().size() == NearbyList::kMax);
    CHECK(list.Find(100) && !list.Find(101) && list.Find(200));
    CHECK(ScanResultSentence(0).rfind(L"No phone found.", 0) == 0);
    CHECK(ScanResultSentence(1) == L"Found 1 phone." && ScanResultSentence(2) == L"Found 2 phones.");
    // Scan rows have stable ids in their own block.
    int part = -1;
    CHECK(ui::ScanRowIndex(ui::ScanRowId(3, 1), &part) == 3 && part == 1);
    CHECK(ui::ScanRowIndex(ui::ScanRowId(15, 0), &part) == 15 && part == 0);
    CHECK(ui::ScanRowIndex(ui::kScanList, &part) == -1 && ui::ScanRowIndex(ui::ScanRowId(16, 0), &part) == -1);
}

void TestPreviewConvert() {
    // 4x2 NV12 -> 2x1 BGRA: each output pixel averages a 2x2 luma block.
    const uint8_t nv12[] = {16, 16, 235, 235, 16, 16, 235, 235, 128, 128, 128, 128};
    uint8_t out[8] = {};
    Nv12ToBgraHalf(nv12, 2, 1, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255);
    CHECK(out[4] == 255 && out[5] == 255 && out[6] == 255 && out[7] == 255);
    // Pure red from BgraToNV12 comes back red.
    std::vector<uint8_t> bgra(4 * 4 * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = 0; bgra[i + 1] = 0; bgra[i + 2] = 255; bgra[i + 3] = 255; }
    std::vector<uint8_t> yuv(4 * 4 * 3 / 2);
    BgraToNV12(bgra.data(), 16, 4, 4, yuv.data());
    uint8_t back[2 * 2 * 4];
    Nv12ToBgraHalf(yuv.data(), 2, 2, back);
    CHECK(back[2] > 240 && back[1] < 15 && back[0] < 15);
}

void TestTrayStatusLine() {
    TrayStatus s;
    s.icon = kIconStreaming;
    s.canPause = s.linked = s.mirror = true;
    s.facing = proto::kFacingFront;
    s.quality = proto::kQuality4K;
    s.qualityOk[0] = s.qualityOk[2] = true;
    s.headline = "Streaming";
    s.detail = "Front camera \xC2\xB7 3840 \xC3\x97 2160 \t from \"My\tPhone\""; // UTF-8, and a tab in a name.
    const std::string line = FormatTrayStatus(s);
    CHECK(line.find('\n') == std::string::npos && line.rfind("status icon=103 ", 0) == 0);
    TrayStatus back;
    CHECK(ParseTrayStatus(line, &back));
    CHECK(back.icon == s.icon && back.canPause && !back.paused && back.linked && back.facing == s.facing);
    CHECK(back.quality == s.quality && back.qualityOk[0] && !back.qualityOk[1] && back.qualityOk[2]);
    CHECK(back.mirror && !back.fill && back.headline == "Streaming");
    CHECK(back.detail == "Front camera \xC2\xB7 3840 \xC3\x97 2160   from \"My Phone\"");
    CHECK(!ParseTrayStatus("status icon=103", &back) && !ParseTrayStatus("url https://x\ta\tb", &back));
}

void TestStatusText() {
    LinkStatus s;
    s.state = LinkState::Waiting;
    s.wireless = true;
    s.phoneName = L"Pixel 8";
    s.pairingCode = L"554 294";
    StatusView v = DescribeStatus(s, true);
    CHECK(v.headline == L"Pairing with Pixel 8…");
    CHECK(v.progress == StatusProgress::Pairing && v.facts.find(L"554 294") != std::wstring::npos);
    s.pairingCode.clear();
    CHECK(DescribeStatus(s, true).progress == StatusProgress::Working);
    s.state = LinkState::Streaming;
    s.width = 1920;
    s.height = 1080;
    v = DescribeStatus(s, true);
    CHECK(v.headline == L"Streaming" && v.progress == StatusProgress::None && v.icon == kIconStreaming);
    CHECK(v.facts == L"Back camera · 1920 × 1080 · Wi-Fi");
    s.state = LinkState::Idle;
    CHECK(DescribeStatus(s, true).progress == StatusProgress::None); // Never in steady states.
    s.lockPaused = true;
    CHECK(DescribeStatus(s, true).icon == kIconPaused);
    CHECK(DescribeStatus(s, false).icon == kIconError);
    // An iPhone's web page says how it reaches the PC.
    s.lockPaused = false;
    s.web = true;
    s.webLink = L"USB hotspot";
    s.state = LinkState::Streaming;
    CHECK(DescribeStatus(s, true).facts == L"Back camera · 1920 × 1080 · USB hotspot");
    s.state = LinkState::Idle;
    CHECK(DescribeStatus(s, true).facts == L"Back camera · USB hotspot");
    s.state = LinkState::Paused;
    s.phoneName = L"iPhone";
    s.webAway = true; // The page is locked or in the background: it, not the PC, can resume.
    CHECK(DescribeStatus(s, true).hint.find(L"Open it again") != std::wstring::npos);

    // Windows 10 (docs/WINDOWS10-SUPPORT.md): a neutral not-registered message (no OS claimed), and the
    // DirectShow backend tagged onto the facts line but not the headline or tray tooltip.
    s.state = LinkState::Streaming;
    s.web = false;
    s.webAway = false;
    s.wireless = false;
    CHECK(DescribeStatus(s, false).headline == L"Camera not registered");
    CHECK(DescribeStatus(s, false).detail.find(L"Windows 11") == std::wstring::npos);
    v = DescribeStatus(s, true, L"DirectShow");
    CHECK(v.facts == L"Back camera · 1920 × 1080 · USB (DirectShow)");
    CHECK(v.detail == L"Back camera · 1920 × 1080");
    CHECK(DescribeStatus(s, true).facts == L"Back camera · 1920 × 1080 · USB"); // Frame Server: no tag.
}

// --- iPhone web camera (web_proto.h) -------------------------------------------------------------------

// A client (browser) frame: always masked.
Bytes ClientFrame(bool fin, uint8_t opcode, const Bytes& payload, const uint8_t mask[4] = nullptr) {
    static const uint8_t kMask[4] = {0x37, 0xFA, 0x21, 0x3D};
    if (!mask) mask = kMask;
    Bytes f = {uint8_t((fin ? 0x80 : 0) | opcode)};
    const size_t n = payload.size();
    if (n < 126) {
        f.push_back(uint8_t(0x80 | n));
    } else if (n < 65536) {
        f.push_back(0x80 | 126);
        f.push_back(uint8_t(n >> 8));
        f.push_back(uint8_t(n));
    } else {
        f.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) f.push_back(uint8_t(uint64_t(n) >> (8 * i)));
    }
    f.insert(f.end(), mask, mask + 4);
    for (size_t i = 0; i < n; ++i) f.push_back(uint8_t(payload[i] ^ mask[i & 3]));
    return f;
}

struct WsMsg { uint8_t opcode; Bytes data; };

bool FeedWs(web::WsParser& p, const Bytes& data, std::vector<WsMsg>* out, size_t chunk = 0) {
    auto on = [&](uint8_t op, const uint8_t* d, size_t n) { out->push_back({op, Bytes(d, d + n)}); };
    if (chunk == 0) return p.Feed(data.data(), data.size(), on);
    for (size_t i = 0; i < data.size(); i += chunk) {
        if (!p.Feed(data.data() + i, std::min(chunk, data.size() - i), on)) return false;
    }
    return true;
}

void TestWebSocketFrames() {
    // RFC 6455 §5.7: a masked "Hello".
    {
        web::WsParser p;
        std::vector<WsMsg> got;
        CHECK(FeedWs(p, Hex("81 85 37 fa 21 3d 7f 9f 4d 51 58"), &got));
        CHECK(got.size() == 1 && got[0].opcode == web::kText && got[0].data == Bytes({'H', 'e', 'l', 'l', 'o'}));
    }
    // Every length encoding, fed whole and one byte at a time.
    for (size_t n : {size_t(0), size_t(125), size_t(126), size_t(300), size_t(65535), size_t(65536), size_t(200000)}) {
        Bytes payload(n);
        for (size_t i = 0; i < n; ++i) payload[i] = uint8_t(i * 7 + 3);
        const Bytes frame = ClientFrame(true, web::kBinary, payload);
        for (size_t chunk : {size_t(0), size_t(1), size_t(4093)}) {
            if (chunk == 1 && n > 70000) continue; // Slow and adds nothing.
            web::WsParser p;
            std::vector<WsMsg> got;
            CHECK(FeedWs(p, frame, &got, chunk));
            CHECK(got.size() == 1 && got[0].opcode == web::kBinary && got[0].data == payload);
        }
    }
    // Two messages in one read, then the start of a third.
    {
        web::WsParser p;
        std::vector<WsMsg> got;
        Bytes both = ClientFrame(true, web::kBinary, {1, 2, 3});
        const Bytes second = ClientFrame(true, web::kBinary, {4, 5});
        both.insert(both.end(), second.begin(), second.end());
        const Bytes third = ClientFrame(true, web::kBinary, {6});
        both.insert(both.end(), third.begin(), third.begin() + 3);
        CHECK(FeedWs(p, both, &got) && got.size() == 2 && got[1].data == Bytes({4, 5}));
        CHECK(FeedWs(p, Bytes(third.begin() + 3, third.end()), &got) && got.size() == 3 && got[2].data == Bytes({6}));
    }
    // A fragmented message with a ping in the middle: the ping comes out first, then the joined message.
    {
        web::WsParser p;
        std::vector<WsMsg> got;
        Bytes all = ClientFrame(false, web::kBinary, {1, 2});
        for (const Bytes& f : {ClientFrame(true, web::kPing, {9}), ClientFrame(false, web::kContinuation, {3}),
                               ClientFrame(true, web::kContinuation, {4, 5})})
            all.insert(all.end(), f.begin(), f.end());
        CHECK(FeedWs(p, all, &got, 3));
        CHECK(got.size() == 2 && got[0].opcode == web::kPing && got[0].data == Bytes({9}));
        CHECK(got[1].opcode == web::kBinary && got[1].data == Bytes({1, 2, 3, 4, 5}));
    }
    // Close and pong are passed on.
    {
        web::WsParser p;
        std::vector<WsMsg> got;
        CHECK(FeedWs(p, ClientFrame(true, web::kClose, {0x03, 0xE8}), &got) && got.size() == 1 &&
              got[0].opcode == web::kClose && got[0].data == Bytes({0x03, 0xE8}));
    }
    // Protocol errors.
    auto fails = [](const Bytes& data) {
        web::WsParser p;
        std::vector<WsMsg> got;
        return !FeedWs(p, data, &got) && p.Error() != nullptr && got.empty();
    };
    CHECK(fails(Hex("82 03 01 02 03")));                                // Not masked.
    CHECK(fails(ClientFrame(true, web::kContinuation, {1})));           // Continuation with nothing to continue.
    CHECK(fails(ClientFrame(false, web::kPing, {1})));                  // Fragmented control frame.
    CHECK(fails(ClientFrame(true, web::kPing, Bytes(126))));            // Control frame over 125 bytes.
    CHECK(fails(ClientFrame(true, 3, {1})));                            // Reserved opcode.
    {
        Bytes rsv = ClientFrame(true, web::kBinary, {1});
        rsv[0] |= 0x40;                                                  // RSV1 (compression) wasn't agreed.
        CHECK(fails(rsv));
    }
    {
        Bytes twoStarts = ClientFrame(false, web::kBinary, {1});
        const Bytes again = ClientFrame(true, web::kBinary, {2});
        twoStarts.insert(twoStarts.end(), again.begin(), again.end());
        CHECK(fails(twoStarts));
    }
    {
        // A header announcing more than one MyCam packet can hold is refused before the body arrives.
        web::WsParser p;
        std::vector<WsMsg> got;
        CHECK(!FeedWs(p, Hex("82 FF 00 00 00 00 01 00 00 00"), &got) && p.TooBig());
        CHECK(!p.Feed(nullptr, 0, [](uint8_t, const uint8_t*, size_t) {})); // Stays failed.
        p.Reset();
        CHECK(FeedWs(p, ClientFrame(true, web::kBinary, {7}), &got) && got.size() == 1);
    }
    // Server frames: unmasked, FIN, the shortest length encoding.
    CHECK(web::Frame(web::kBinary, nullptr, 0) == Hex("82 00"));
    {
        const Bytes cmd = Hex("4D434D44 02 00 0000");
        CHECK(web::Frame(web::kBinary, cmd.data(), cmd.size()) == Hex("82 08 4D434D44 02 00 0000"));
        const Bytes b126(126, 0xAB), b65536(65536, 0xCD);
        const Bytes f126 = web::Frame(web::kBinary, b126.data(), b126.size());
        CHECK(f126.size() == 4 + 126 && f126[1] == 126 && f126[2] == 0 && f126[3] == 126);
        const Bytes f64k = web::Frame(web::kBinary, b65536.data(), b65536.size());
        CHECK(f64k.size() == 10 + 65536 && f64k[1] == 127 && f64k[7] == 1 && f64k[8] == 0 && f64k[9] == 0);
    }
    CHECK(web::CloseFrame(1000) == Hex("88 02 03 E8"));
}

void TestWebHandshake() {
    // RFC 6455 §1.3.
    CHECK(web::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    const std::string upgrade =
        "GET /ws?d=abc%2Ddef&x=1 HTTP/1.1\r\n"
        "Host: 192.168.1.20:47900\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://192.168.1.20:47900\r\n"
        "\r\n";
    const Bytes raw(upgrade.begin(), upgrade.end());
    web::HttpRequest r;
    size_t head = 0;
    CHECK(web::ParseHttpRequest(raw.data(), raw.size() - 1, &r, &head) == web::HttpParse::Incomplete);
    CHECK(web::ParseHttpRequest(raw.data(), raw.size(), &r, &head) == web::HttpParse::Done);
    CHECK(head == raw.size() && r.method == "GET" && r.path == "/ws" && r.query == "d=abc%2Ddef&x=1");
    CHECK(web::QueryParam(r.query, "d") == "abc-def" && web::QueryParam(r.query, "x") == "1");
    CHECK(web::QueryParam(r.query, "t").empty());
    CHECK(r.Header("host") == "192.168.1.20:47900" && r.Header("upgrade") == "websocket");
    CHECK(web::IsWebSocketUpgrade(r));
    web::HttpRequest bad = r;
    bad.headers["connection"] = "keep-alive";
    CHECK(!web::IsWebSocketUpgrade(bad));
    bad = r;
    bad.headers["sec-websocket-version"] = "8";
    CHECK(!web::IsWebSocketUpgrade(bad));
    bad = r;
    bad.headers["sec-websocket-key"] = "short==";
    CHECK(!web::IsWebSocketUpgrade(bad));
    bad = r;
    bad.method = "POST";
    CHECK(!web::IsWebSocketUpgrade(bad));

    // A POST with a body; the body is left for the caller.
    const std::string post = "POST /pair HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"t\":\"abc\"}";
    const Bytes p(post.begin(), post.end());
    CHECK(web::ParseHttpRequest(p.data(), p.size(), &r, &head) == web::HttpParse::Done);
    CHECK(r.method == "POST" && r.path == "/pair" && r.contentLength == 11 && p.size() - head == 11);

    auto parse = [&](const std::string& text) {
        const Bytes b(text.begin(), text.end());
        return web::ParseHttpRequest(b.data(), b.size(), &r, &head);
    };
    CHECK(parse("GET / HTTP/1.1\r\n\r\n") == web::HttpParse::Done && r.path == "/");
    CHECK(parse("GARBAGE\r\n\r\n") == web::HttpParse::Bad);
    CHECK(parse("GET nothing HTTP/1.1\r\n\r\n") == web::HttpParse::Bad);
    CHECK(parse("GET / HTTP/1.1\r\nNoColon\r\n\r\n") == web::HttpParse::Bad);
    CHECK(parse("POST /pair HTTP/1.1\r\nContent-Length: 99999\r\n\r\n") == web::HttpParse::Bad);
    CHECK(parse("POST /pair HTTP/1.1\r\nContent-Length: -1\r\n\r\n") == web::HttpParse::Bad);
    CHECK(parse("POST /pair HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == web::HttpParse::Bad);
    CHECK(parse(std::string("GET / HTTP/1.1\r\nX: ") + std::string(9000, 'a')) == web::HttpParse::Bad);
}

void TestWebTokens() {
    web::TokenBook book;
    const Bytes r16(16, 0x11), r16b(16, 0x22), r32(32, 0x33), r8 = Hex("0102030405060708");
    const std::string code = book.NewPairingCode(r16, 1000);
    CHECK(code.size() == 22 && code.find_first_of("+/=") == std::string::npos); // 128 bits, URL-safe.
    CHECK(book.HasPairingCode(1000) && !book.HasPairingCode(1000 + web::kPairingCodeMs));
    CHECK(!book.UsePairingCode("wrong", 2000));
    CHECK(!book.UsePairingCode(code + "x", 2000));
    CHECK(!book.UsePairingCode(code, 1000 + web::kPairingCodeMs)); // Expired.
    CHECK(book.UsePairingCode(code, 2000));
    CHECK(!book.UsePairingCode(code, 2001));                      // Single use.
    CHECK(!book.HasPairingCode(2001));
    const std::string second = book.NewPairingCode(r16b, 3000);
    CHECK(second != code && !book.UsePairingCode(code, 3001));     // The old code is gone.
    book.ClearPairingCode();
    CHECK(!book.UsePairingCode(second, 3002));

    web::WebDevice added;
    const std::string token = book.AddDevice("Dad's iPhone\n", r32, r8, 1760000000, &added);
    CHECK(token.size() == 43 && added.id == "0102030405060708" && added.name == "Dad's iPhone");
    CHECK(added.tokenHash.size() == 32 && std::string(added.tokenHash.begin(), added.tokenHash.end()) != token);
    const web::WebDevice* found = book.FindDevice(token);
    CHECK(found && found->id == added.id);
    CHECK(!book.FindDevice(token.substr(1)) && !book.FindDevice(""));
    // Restored from storage (hash only), it still matches.
    web::TokenBook restored;
    restored.SetDevices({added});
    CHECK(restored.FindDevice(token) != nullptr);
    CHECK(restored.Forget(added.id) && !restored.FindDevice(token) && !restored.Forget(added.id));

    CHECK(web::CleanDeviceName("") == "iPhone");
    CHECK(web::CleanDeviceName("  iPad\t") == "iPad");
    CHECK(web::CleanDeviceName(std::string(60, 'x')).size() == 40);
    CHECK(web::CleanDeviceName("Café ☕") == "Café ☕");
    CHECK(web::CleanDeviceName(std::string("ab\xff") + "cd") == "ab"); // Stops at bad UTF-8.

    // CleanName: what a phone sends reaches the pairing dialog's title, so nothing in it may leave its line.
    CHECK(web::CleanName("Pixel 9", "fallback") == "Pixel 9");
    CHECK(web::CleanName("", "192.168.1.9") == "192.168.1.9");            // Empty falls back.
    CHECK(web::CleanName("  \t ", "fallback") == "fallback");             // So does all-whitespace.
    CHECK(web::CleanName("A\nAllow this", "x") == "AAllow this");         // Newlines go: they made extra lines.
    CHECK(web::CleanName("A\r\nB", "x") == "AB");
    CHECK(web::CleanName("A B", "x") == "AB");                       // Line separator.
    CHECK(web::CleanName("A B", "x") == "AB");                       // Paragraph separator.
    CHECK(web::CleanName("A‮B", "x") == "AB");                       // Right-to-left override.
    CHECK(web::CleanName("A⁦B⁩C", "x") == "ABC");               // Isolates.
    CHECK(web::CleanName("A﻿B", "x") == "AB");
    CHECK(web::CleanName("Café ☕", "x") == "Café ☕");                   // Ordinary text is untouched.
    CHECK(web::CleanName(std::string(60, 'x'), "y").size() == 40);        // Still capped.
    CHECK(web::CleanDeviceName("") == "iPhone");                          // The page's default is unchanged.

    CHECK(web::SameSecret("abc", "abc") && !web::SameSecret("abc", "abd") && !web::SameSecret("abc", "ab"));
    const Bytes hello = {'h', 'i', '?'};
    CHECK(web::Base64(hello.data(), 3) == "aGk/" && web::Base64Url(hello.data(), 3) == "aGk_");
    CHECK(web::Base64(hello.data(), 2) == "aGk=" && web::Base64Url(hello.data(), 2) == "aGk");
}

void TestWebAddresses() {
    using web::NetKind;
    auto ip = [](int a, int b, int c, int d) { return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | uint32_t(d); };
    CHECK(web::ClassifyAdapter(71, L"Intel(R) Wi-Fi 6 AX201", ip(192, 168, 1, 20)) == NetKind::WiFi);
    CHECK(web::ClassifyAdapter(6, L"Realtek PCIe GbE", ip(10, 0, 0, 5)) == NetKind::Ethernet);
    CHECK(web::ClassifyAdapter(6, L"Apple Mobile Device Ethernet", ip(172, 20, 10, 2)) == NetKind::IPhoneUsb);
    CHECK(web::ClassifyAdapter(6, L"Hyper-V Virtual Ethernet Adapter", ip(172, 28, 0, 1)) == NetKind::Skip);
    CHECK(web::ClassifyAdapter(6, L"vEthernet (WSL)", ip(172, 29, 0, 1)) == NetKind::Skip);
    CHECK(web::ClassifyAdapter(53, L"Cloudflare WARP Interface Tunnel", ip(172, 16, 0, 2)) == NetKind::Other);
    CHECK(web::ClassifyAdapter(6, L"TAP-Windows Adapter V9", ip(10, 8, 0, 2)) == NetKind::Other);
    CHECK(web::ClassifyAdapter(71, L"Wi-Fi", ip(169, 254, 3, 4)) == NetKind::Skip);
    CHECK(web::ClassifyAdapter(24, L"Loopback", ip(127, 0, 0, 1)) == NetKind::Skip);

    std::vector<web::NetAddress> list = {{1, "vpn", L"", NetKind::Other}, {2, "eth", L"", NetKind::Ethernet},
                                         {3, "vm", L"", NetKind::Skip}, {4, "usb", L"", NetKind::IPhoneUsb},
                                         {5, "wifi", L"", NetKind::WiFi}};
    web::SortAddresses(&list);
    CHECK(list.size() == 4 && list[0].text == "wifi" && list[1].text == "eth" && list[2].text == "usb" &&
          list[3].text == "vpn");
}

// QR pairing (PROTOCOL.md "QR pairing"): the key the scanned code goes into, the invite's proof and its
// parsing, and the base64url decoding of the code. Golden "qr.*" (JDK); qr.code_text is text, spelled out here.
void TestQrPairing(std::map<std::string, Bytes>& golden) {
    const Bytes code = golden["qr.code"], port = golden["qr.port"];
    CHECK(code.size() == 16 && port.size() == 2);
    CHECK(wifi::QrPairKey(golden["wifi.ecdh"], code, golden["wifi.npc"], golden["wifi.nph"]) == golden["qr.pair_key"]);
    const uint16_t tcpPort = uint16_t(port[0] << 8 | port[1]);
    const Bytes proofBytes = golden["qr.invite_proof"];
    std::string proofHex;
    for (uint8_t b : proofBytes) { char two[3]; snprintf(two, sizeof(two), "%02x", b); proofHex += two; }
    CHECK(wifi::QrInviteProof(code, tcpPort) == proofHex);
    CHECK(wifi::QrInviteProof(code, uint16_t(tcpPort + 1)) != proofHex); // The port is part of it.

    web::Bytes out;
    CHECK(web::FromBase64Url("YGFiY2RlZmdoaWprbG1ubw", &out) && out == code); // golden "qr.code_text"
    CHECK(web::Base64Url(code.data(), code.size()) == "YGFiY2RlZmdoaWprbG1ubw");
    CHECK(web::FromBase64Url("", &out) && out.empty());
    CHECK(web::FromBase64Url("_-8", &out) && out == (web::Bytes{0xFF, 0xEF})); // The two URL-safe characters.
    for (size_t n = 0; n < 40; ++n) { // Round trips at every length (all three tail cases).
        web::Bytes b(n);
        for (size_t i = 0; i < n; ++i) b[i] = uint8_t(i * 37 + 11);
        CHECK(web::FromBase64Url(web::Base64Url(b.data(), b.size()), &out) && out == b);
    }
    CHECK(!web::FromBase64Url("QUJD=", &out)); // Padding isn't base64url here.
    CHECK(!web::FromBase64Url("QUJD+", &out)); // Nor is the standard alphabet.
    CHECK(!web::FromBase64Url("Q", &out));     // A lone character can't be a whole byte.
    CHECK(!web::FromBase64Url("QR", &out));    // Leftover bits set: not how anyone encodes "A".

    // The phone's invite: "MYCAM#1 <port> <proof> <name>".
    uint16_t p = 0;
    std::string proof, name;
    CHECK(ParseDiscoveryQrRequest("MYCAM#1 47800 " + proofHex + " CMF by Nothing Phone 1", &p, &proof, &name));
    CHECK(p == 47800 && proof == proofHex && name == "CMF by Nothing Phone 1");
    std::string upper = proofHex;
    for (char& c : upper) c = char(toupper(c));
    CHECK(ParseDiscoveryQrRequest("MYCAM#1 47800 " + upper, &p, &proof, &name) && proof == proofHex && name.empty());
    CHECK(!ParseDiscoveryQrRequest("MYCAM#1 47800 1234 Phone", &p, &proof, &name));                // Proof too short.
    CHECK(!ParseDiscoveryQrRequest("MYCAM#1 47800 " + proofHex.substr(0, 31) + "z Phone", &p, &proof, &name)); // Not hex.
    CHECK(!ParseDiscoveryQrRequest("MYCAM+1 47800 " + proofHex + " Phone", &p, &proof, &name));     // A different message.
    CHECK(!ParseDiscoveryConnectRequest("MYCAM#1 47800 " + proofHex, &p, &name));
    // The proof and its space don't eat into the room for a name: a QR invite carries the same 64 bytes of
    // name as any other message (it used to be cut to 31).
    const std::string long64(64, 'n');
    CHECK(ParseDiscoveryQrRequest("MYCAM#1 47800 " + proofHex + " " + long64, &p, &proof, &name));
    CHECK(name == long64);
    CHECK(ParseDiscoveryReply("MYCAM!1 47800 " + long64 + "toolong", &p, &name) && name == long64); // Still 64.
}

} // namespace

int main() {
    auto golden = LoadGolden();
    CHECK(!golden.empty());

    auto withGolden = [&](void (*fn)(std::map<std::string, Bytes>&)) { fn(golden); };

    printf("protocol golden packets\n");   withGolden(TestGoldenPackets);
    printf("protocol golden commands\n");  withGolden(TestGoldenCommands);
    printf("protocol golden camera\n");    withGolden(TestGoldenCamera);
    printf("parser splits/batches\n");     withGolden(TestParserSplitsAndBatches);
    printf("wireless crypto\n");           withGolden(TestWifiCrypto);
    printf("parser resyncs\n");            withGolden(TestParserResyncs);
    printf("parser re-entrant\n");         withGolden(TestParserReentrant);
    printf("transform identity\n");        TestIdentity();
    printf("transform rotations\n");       TestRotations();
    printf("transform mirror\n");          TestMirror();
    printf("transform letterbox\n");       TestLetterbox();
    printf("transform fill\n");            TestFill();
    printf("transform downscale\n");       TestDownscale();
    printf("transform pitch\n");           TestPitch();
    printf("bgra to nv12\n");              TestBgraToNV12();
    printf("nv12 to i420\n");              TestNv12ToI420();
#ifdef _WIN32
    printf("dshow media types\n");         TestDShowMediaTypes();
    printf("winusb accessory ids\n");      TestAccessoryInstances();
#endif
    printf("marquee geometry\n");          TestMarqueeGeometry();
    printf("marquee chunk positions\n");   TestMarqueeChunkPositions();
    printf("marquee pause\n");             TestMarqueePause();
    printf("marquee draw 1280x720\n");     TestMarqueeDraw(1280, 720);
    printf("marquee draw 1920x1080\n");    TestMarqueeDraw(1920, 1080);
    printf("marquee draw 854x480\n");      TestMarqueeDraw(854, 480);
    printf("marquee restore\n");           TestMarqueeRestore();
    printf("ui flow layout\n");            TestUiFlow();
    printf("ui motion\n");                 TestUiMotion();
    printf("ui capabilities\n");           TestCapabilities();
    printf("ui element model\n");          TestUiModel();
    printf("wi-fi discovery list\n");      TestDiscovery();
    printf("preview convert\n");           TestPreviewConvert();
    printf("status text\n");               TestStatusText();
    printf("tray status line\n");          TestTrayStatusLine();
    printf("web: websocket frames\n");     TestWebSocketFrames();
    printf("web: http and handshake\n");   TestWebHandshake();
    printf("web: tokens\n");               TestWebTokens();
    printf("web: addresses\n");            TestWebAddresses();
    printf("qr pairing\n");                withGolden(TestQrPairing);

    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

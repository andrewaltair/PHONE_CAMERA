// MyCamCompanion.exe: tray app that registers the "MyCam" virtual camera and bridges the phone over USB.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/guids.h"
#include "app_settings.h"
#include "capabilities.h"
#include "log.h"
#include "pairing_dialog.h"
#include "driver_task.h"
#include "phone_link.h"
#include "protocol.h"
#include "settings_window.h"
#include "status_images.h"
#include "status_text.h"
#include "winusb_bind.h"

using Microsoft::WRL::ComPtr;
using namespace mycam;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;
constexpr UINT WM_NEED_DRIVER = WM_APP + 3;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuSettings = 100,
    kMenuStatus,
    kMenuPause,
    kMenuBack,
    kMenuFront,
    kMenuMirror,
    kMenuFill,
    kMenuReconnect,
    kMenuAutostart,
    kMenuExit,
    kMenuQ720,
    kMenuQ1080,
    kMenuQ4K,
    kMenuScan,
};

HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_tray = {};
PhoneLink* g_link = nullptr;
SettingsWindow* g_settings = nullptr;
PairingDialog* g_pairing = nullptr;
bool g_mirror = false;
bool g_fill = false;
bool g_wireless = false; // "Find phones on Wi-Fi" (beta)

std::mutex g_statusLock;
LinkStatus g_status;
LinkStatus g_shownStatus;
bool g_vcamOk = false;
std::wstring g_backendName; // "Frame Server" (Windows 11) or "DirectShow" (Windows 10); set once at startup.
bool g_sessionLocked = false; // Windows session locked (WTS notifications)
bool g_suspended = false;     // PC going to sleep
int g_trayIcon = 0;

// --demo modes: a made-up scan (see DemoScan).
constexpr UINT_PTR kDemoScanTimer = 7;
void DemoScanDone();

LinkStatus CurrentStatus() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    return g_status;
}

// Tray icons are drawn for 16-32 px; LoadIconMetric picks the right size for the current DPI.
HICON LoadTrayIcon(int id) {
    HICON icon = nullptr;
    if (FAILED(LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), LIM_SMALL, &icon))) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

void Notify(const wchar_t* title, const std::wstring& text) {
    g_tray.uFlags = NIF_INFO;
    wcsncpy_s(g_tray.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_tray.szInfo, text.c_str(), _TRUNCATE);
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void SetWireless(bool on);

void UpdateTray() {
    const LinkStatus s = CurrentStatus();
    const StatusView view = DescribeStatus(s, g_vcamOk, g_backendName);

    g_tray.uFlags = NIF_TIP;
    std::wstring tip = L"MyCam: " + view.headline + L"\n" + view.detail;
    wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
    if (view.icon != g_trayIcon) {
        if (g_tray.hIcon) DestroyIcon(g_tray.hIcon);
        g_tray.hIcon = LoadTrayIcon(view.icon);
        g_tray.uFlags |= NIF_ICON;
        g_trayIcon = view.icon;
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    if (g_settings) g_settings->Refresh();
    // A new pairing code opens the pairing dialog; it closes itself when the pairing resolves.
    if (g_pairing) {
        if (!s.pairingCode.empty() && s.pairingCode != g_shownStatus.pairingCode) g_pairing->Show(g_settings ? g_settings->Hwnd() : nullptr);
        else g_pairing->Refresh();
    }
    // A new pairing worked (QR code, 6-digit code or iPhone): say so, and show the camera. A Wi-Fi phone also
    // turns on "Find phones on Wi-Fi", so it reconnects by itself next time.
    const bool justPaired = s.pairedCount != g_shownStatus.pairedCount && s.pairedCount != 0;
    if (justPaired) {
        if (s.pairedWifi && !g_wireless) {
            // Turning this on is how a paired phone reconnects by itself, but it also makes this PC announce
            // its name on the network every 2 s from now on, so it is written down rather than done quietly.
            Log("wifi: paired a phone over Wi-Fi, so \"Find phones on Wi-Fi\" is now on (Settings turns it off)");
            SetWireless(true);
        }
        if (g_settings) g_settings->ShowCamera();
        if (g_pairing) {
            const HWND owner = g_settings && g_settings->Hwnd() && IsWindowVisible(g_settings->Hwnd()) ? g_settings->Hwnd() : nullptr;
            g_pairing->ShowPaired(s.pairedName, owner);
        }
    }

    // Balloon only on meaningful transitions.
    bool wasConnected = g_shownStatus.state >= LinkState::Idle;
    bool isConnected = s.state >= LinkState::Idle;
    if (s.state == LinkState::NoDriver && g_shownStatus.state != LinkState::NoDriver) {
        Notify(view.headline.c_str(), view.detail);
    } else if (isConnected && !wasConnected && !(g_pairing && g_pairing->IsOpen())) { // Not over "Paired".
        // "iPhone connected over Wi-Fi" for a web page (its name, and how it reaches the PC).
        const std::wstring title = s.web ? (s.phoneName.empty() ? L"iPhone" : s.phoneName) + L" connected over " + s.webLink
                                 : s.wireless ? L"Phone connected over Wi-Fi" : L"Phone connected";
        Notify(title.c_str(), L"Choose “MyCam” as the camera in any app.");
    } else if (!s.pairingCode.empty() && s.pairingCode != g_shownStatus.pairingCode) {
        Notify(view.headline.c_str(), view.detail); // "Pairing with …": clicking it opens the pairing dialog.
    } else if (s.state == LinkState::Waiting && g_shownStatus.state != LinkState::Waiting && !s.wireless) {
        Notify(L"Phone found", L"Tap OK on your phone to open MyCam (tick “Always” so you're never asked again).");
    }
    g_shownStatus = s;
}

void SetMirror(bool mirror) {
    g_mirror = mirror;
    g_link->SetMirror(mirror);
    WriteSetting(L"Mirror", mirror);
}

void SetFill(bool fill) {
    g_fill = fill;
    g_link->SetFill(fill);
    WriteSetting(L"Fill", fill);
}

void SetWireless(bool on) {
    g_wireless = on;
    g_link->SetWireless(on);
    WriteSetting(L"Wireless", on);
}

// Windows locked or asleep: keep the phone camera off (privacy). Separate from the user's own pause.
void UpdateLockPause() {
    if (g_link) g_link->SetLockPaused(g_sessionLocked || g_suspended);
}

void ShowMenu(HWND hwnd) {
    const LinkStatus s = CurrentStatus();
    const StatusView view = DescribeStatus(s, g_vcamOk, g_backendName);
    HMENU menu = CreatePopupMenu();
    // The status line comes first (docs/redesign.md §8.5), then the window.
    std::wstring status = L"MyCam: " + view.headline;
    for (size_t at = 0; (at = status.find(L'&', at)) != std::wstring::npos; at += 2) status.insert(at, 1, L'&');
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"&Open MyCam");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE); // Bold; also what a left-click opens.
    const bool paused = s.state == LinkState::Paused;
    const bool connected = s.state >= LinkState::Idle;
    AppendMenuW(menu, MF_STRING | (connected && !s.lockPaused && !s.webAway ? 0 : MF_GRAYED), kMenuPause,
                paused ? L"&Resume camera" : L"&Pause camera");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuBack, L"&Back camera");
    AppendMenuW(menu, MF_STRING, kMenuFront, L"&Front camera");
    CheckMenuRadioItem(menu, kMenuBack, kMenuFront, s.facing == proto::kFacingFront ? kMenuFront : kMenuBack, MF_BYCOMMAND);
    // Quality, greyed by what the phone reports, exactly like the window.
    HMENU quality = CreatePopupMenu();
    const bool linked = connected && s.camera.valid;
    const struct { UINT id; uint8_t q; const wchar_t* label; } qualities[] = {
        {kMenuQ720, proto::kQuality720p, L"720p"}, {kMenuQ1080, proto::kQuality1080p, L"1080p"}, {kMenuQ4K, proto::kQuality4K, L"4K"}};
    for (const auto& q : qualities) {
        AppendMenuW(quality, MF_STRING | (caps::QualityEnabled(linked, s.camera, q.q) ? 0 : MF_GRAYED), q.id, q.label);
    }
    if (linked && s.camera.quality <= proto::kQuality4K) {
        CheckMenuRadioItem(quality, kMenuQ720, kMenuQ4K, kMenuQ720 + s.camera.quality, MF_BYCOMMAND);
    }
    AppendMenuW(menu, MF_POPUP | (linked ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(quality), L"&Quality");
    AppendMenuW(menu, MF_STRING | (g_mirror ? MF_CHECKED : 0), kMenuMirror, L"&Mirror image");
    AppendMenuW(menu, MF_STRING | (g_fill ? MF_CHECKED : 0), kMenuFill, L"Fill the frame");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuReconnect, L"Reconnect phone");
    AppendMenuW(menu, MF_STRING, kMenuScan, L"&Scan for phones"); // Opens the window and scans the Wi-Fi.
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Runs "MyCamCompanion.exe --bind-driver" elevated to give an accessory-mode phone the WinUSB driver.
// Normally through the installer's scheduled task (no prompt); if that's missing, Windows asks for admin
// once per phone. Either way the binding persists.
void RunDriverBinder() {
    if (RunDriverTask()) return;
    static HANDLE running = nullptr;
    if (running) {
        if (WaitForSingleObject(running, 0) == WAIT_TIMEOUT) return;
        CloseHandle(running);
        running = nullptr;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = L"--bind-driver";
    sei.nShow = SW_HIDE;
    Notify(L"Setting up your phone", L"Windows will ask for permission once to install the USB driver for this phone.");
    if (ShellExecuteExW(&sei)) running = sei.hProcess;
    else Notify(L"MyCam", L"Permission was declined. Unplug and replug the phone to try again.");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP: if (g_settings) g_settings->Show(); break;
        case NIN_BALLOONUSERCLICK: // The pairing balloon opens the pairing dialog; any other opens the window.
            if (g_pairing && !CurrentStatus().pairingCode.empty()) g_pairing->Show(g_settings ? g_settings->Hwnd() : nullptr);
            else if (g_settings) g_settings->Show();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: ShowMenu(hwnd); break;
        }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_UNLOCK) {
            g_sessionLocked = wp == WTS_SESSION_LOCK;
            UpdateLockPause();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) g_suspended = true;
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) g_suspended = false;
        UpdateLockPause();
        return TRUE;
    case WM_NEED_DRIVER:
        RunDriverBinder();
        return 0;
    case WM_TIMER:
        if (wp == kDemoScanTimer) DemoScanDone();
        return 0;
    case WM_STATUS:
        UpdateTray();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuSettings: if (g_settings) g_settings->Show(); break;
        case kMenuPause: g_link->RequestPause(CurrentStatus().state != LinkState::Paused); break;
        case kMenuBack:  g_link->RequestFacing(proto::kFacingBack); break;
        case kMenuFront: g_link->RequestFacing(proto::kFacingFront); break;
        case kMenuMirror: SetMirror(!g_mirror); break;
        case kMenuFill: SetFill(!g_fill); break;
        case kMenuQ720: case kMenuQ1080: case kMenuQ4K:
            g_link->RequestCommand(proto::kCmdSetQuality, uint8_t(LOWORD(wp) - kMenuQ720));
            break;
        case kMenuReconnect: g_link->RequestReconnect(); break;
        case kMenuScan: if (g_settings) g_settings->StartScan(); break;
        case kMenuAutostart: SetAutostart(!AutostartEnabled()); break;
        case kMenuExit: DestroyWindow(hwnd); break;
        }
        if (g_settings) g_settings->Refresh();
        return 0;
    case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
        return 0;
    }
    // Explorer restarted: re-add the tray icon.
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &g_tray);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --test-pattern: feeds moving colour bars instead of the phone, to check the virtual camera alone.
void RunTestPattern(std::atomic<bool>& quit, uint32_t rotation) {
    constexpr uint32_t w = 1280, h = 720;
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                       {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    std::vector<uint8_t> frame(w * h * 3 / 2);
    FrameWriter writer;
    for (uint32_t n = 0; !quit; ++n) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) frame[y * w + x] = bars[(x * 8 / w + n / 30) % 8][0];
        for (uint32_t y = 0; y < h / 2; ++y)
            for (uint32_t x = 0; x < w / 2; ++x) {
                const uint8_t* c = bars[(x * 16 / w + n / 30) % 8];
                frame[w * h + y * w + x * 2] = c[1];
                frame[w * h + y * w + x * 2 + 1] = c[2];
            }
        uint32_t line = n * 8 % h; // White line sweeping down shows the feed is live.
        memset(&frame[line * w], 235, w);
        writer.Write(frame.data(), w, h, rotation, false, g_fill);
        Sleep(33);
    }
}

// Made-up phones for the scan demos (addresses in network order). `firstConnected`: the first one is the
// phone of the current Wi-Fi session.
std::vector<NearbyPhone> DemoPhones(bool firstConnected) {
    auto phone = [](uint8_t last, const char* name, bool paired) {
        NearbyPhone p;
        p.ip = 192u | 168u << 8 | 1u << 16 | uint32_t(last) << 24;
        p.ipText = "192.168.1." + std::to_string(last);
        p.port = 47800;
        p.name = name;
        p.paired = paired;
        return p;
    };
    std::vector<NearbyPhone> out = {phone(23, "Pixel 8", true), phone(41, "Galaxy S23 Ultra (the big one in the kitchen)", false),
                                    phone(57, "moto g54", false)};
    out[0].connected = firstConnected;
    return out;
}

// --demo=<state> (developer use, with or without --test-pattern): shows the window in a made-up link state
// (ready, streaming, paused, pairing, waiting, error, scanning, scan-results, scan-none, scan-wifi) without
// a phone, for checking the UI (also iphone: the "Connect an iPhone" page). The phone link doesn't run.
bool DemoStatus(LinkStatus* s) {
    const wchar_t* arg = wcsstr(GetCommandLineW(), L"--demo=");
    if (!arg) return false;
    const std::wstring state(arg + 7, wcscspn(arg + 7, L" \""));
    proto::CameraInfo& c = s->camera;
    c.valid = true;
    c.quality = proto::kQuality1080p;
    c.fps = 30;
    c.zoomX100 = 100; c.zoomMinX100 = 60; c.zoomMaxX100 = 500;
    c.ev = 1; c.evMin = -4; c.evMax = 4; c.evStepX100 = 50;
    c.flags = proto::kCamTorchAvailable | proto::kCamHas4K | proto::kCamHasAutofocus | proto::kCamHas60Fps;
    c.width = 1920; c.height = 1080; c.actualFps = 30;
    c.hasFpsModes = true;
    c.fpsModes[0] = proto::kFps30Bit | proto::kFps60Bit | proto::kFps120Bit;
    c.fpsModes[1] = proto::kFps30Bit | proto::kFps60Bit;
    c.fpsModes[2] = proto::kFps30Bit;
    s->facing = proto::kFacingBack;
    if (state == L"streaming") { s->state = LinkState::Streaming; s->width = 1920; s->height = 1080; }
    else if (state == L"paused") { s->state = LinkState::Paused; c.width = 0; }
    else if (state == L"ready") { s->state = LinkState::Idle; c.width = 0; }
    else if (state == L"error") { s->state = LinkState::PhoneError; }
    else if (state == L"waiting") { s->state = LinkState::Waiting; c = {}; }
    else if (state == L"pairing") {
        s->state = LinkState::Waiting;
        c = {};
        s->wireless = s->wirelessSearch = true;
        s->phoneName = L"Pixel 8 Pro (Saif's phone with a rather long name)";
        s->pairingCode = L"554 294";
    } else if (state == L"iphone") { // "Connect an iPhone": a made-up QR code, two networks, two remembered iPhones.
        s->state = LinkState::Idle;
        c.width = 0;
        s->wireless = s->web = true;
        s->webLink = L"Wi-Fi";
        s->phoneName = L"iPhone";
        s->webReady = true;
        s->webUrl = L"https://192.168.1.20:47900/?t=demo-not-a-real-code0";
        web::NetAddress wifi, eth;
        wifi.ipv4 = 0x1401A8C0; wifi.text = "192.168.1.20"; wifi.adapter = L"Wi-Fi"; wifi.kind = web::NetKind::WiFi;
        eth.ipv4 = 0x0500000A; eth.text = "10.0.0.5"; eth.adapter = L"Ethernet 2"; eth.kind = web::NetKind::Ethernet;
        s->webAddresses = {wifi, eth};
        s->webAddress = wifi.ipv4;
        s->webDevices = {{"a1", L"iPhone", 1760140800, true}, {"b2", L"iPad", 1759000000, false}};
    } else if (state == L"scan-wifi") { // Connected over Wi-Fi: the scan shows in the Wi-Fi group.
        s->state = LinkState::Idle;
        c.width = 0;
        s->wireless = s->wirelessSearch = true;
        s->phoneName = L"Pixel 8";
        s->scansDone = 1;
        s->nearbyPhones = DemoPhones(true);
    } else {
        s->state = LinkState::Searching;
        c = {};
        if (state == L"scanning") { // A scan running, one phone answered so far.
            s->scanning = true;
            s->nearbyPhones = DemoPhones(false);
            s->nearbyPhones.resize(1);
        } else if (state == L"scan-results") {
            s->scansDone = 1;
            s->nearbyPhones = DemoPhones(false);
        } else if (state == L"scan-none") {
            s->scansDone = 1;
        }
    }
    return true;
}

// Demo mode: "Scan for phones" runs a made-up 6 s scan that finds the demo phones.
void DemoScan() {
    {
        std::lock_guard<std::mutex> lock(g_statusLock);
        g_status.scanning = true;
        g_status.nearbyPhones.clear();
    }
    PostMessageW(g_hwnd, WM_STATUS, 0, 0);
    SetTimer(g_hwnd, kDemoScanTimer, UINT(PhoneLink::kScanWindowMs), nullptr);
}

void DemoScanDone() {
    KillTimer(g_hwnd, kDemoScanTimer);
    {
        std::lock_guard<std::mutex> lock(g_statusLock);
        g_status.scanning = false;
        ++g_status.scansDone;
        g_status.nearbyPhones = DemoPhones(g_status.wireless);
        if (wcsstr(GetCommandLineW(), L"--demo=scan-none")) g_status.nearbyPhones.clear();
    }
    PostMessageW(g_hwnd, WM_STATUS, 0, 0);
}

int QuitRunningInstance() {
    // Current versions use a hidden top-level window; versions before 1.1 used a message-only window.
    HWND other = FindWindowW(L"MyCamCompanion", nullptr);
    if (!other) other = FindWindowExW(HWND_MESSAGE, nullptr, L"MyCamCompanion", nullptr);
    if (!other) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    DWORD wait = process ? WaitForSingleObject(process, 5000) : WAIT_FAILED;
    if (process) CloseHandle(process);
    return wait == WAIT_OBJECT_0 ? 0 : 1;
}

// True on Windows 11 (build 22000+), where the Frame Server virtual camera API exists. RtlGetVersion
// (unlike GetVersionExW) always reports the true build number, with no manifest-driven app-compat lying.
bool IsWindows11OrGreater() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW info = {sizeof(info)};
    return fn && fn(&info) == 0 && info.dwBuildNumber >= 22000;
}

// MFCreateVirtualCamera is looked up at runtime, not linked statically: mfsensorgroup.dll (or this
// export on it) may not exist pre-Windows 11, and this only ever runs after IsWindows11OrGreater().
HRESULT CreateVirtualCamera(ComPtr<IMFVirtualCamera>& vcam) {
    using MFCreateVirtualCameraFn = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, MFVirtualCameraLifetime,
                                                              MFVirtualCameraAccess, LPCWSTR, LPCWSTR, const GUID*,
                                                              ULONG, IMFVirtualCamera**);
    HMODULE module = LoadLibraryW(L"mfsensorgroup.dll");
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    auto create = reinterpret_cast<MFCreateVirtualCameraFn>(GetProcAddress(module, "MFCreateVirtualCamera"));
    if (!create) return E_NOTIMPL;
    HRESULT hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                        MFVirtualCameraAccess_CurrentUser, kCameraFriendlyName, MYCAM_VCAM_CLSID_STRING,
                        nullptr, 0, &vcam);
    if (SUCCEEDED(hr)) hr = vcam->Start(nullptr);
    return hr;
}

// Windows 10 (see docs/WINDOWS10-SUPPORT.md): no Frame Server, so MyCamVCamDShow.dll is a DirectShow
// source filter instead, registered once by the installer (regsvr32 + IFilterMapper2) rather than
// activated per session. "Registered" here just means that install step landed.
bool IsDShowCameraRegistered() {
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + MYCAM_VCAM_DSHOW_CLSID_STRING + L"\\InprocServer32";
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS) return false;
    RegCloseKey(hkey);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Elevated helper mode (launched by the companion via UAC): bind WinUSB to accessory-mode phones.
    if (wcsstr(GetCommandLineW(), L"--bind-driver")) return BindAccessoryDrivers();
    // Installer (elevated): register the task that lets later phones be set up without a UAC prompt.
    if (wcsstr(GetCommandLineW(), L"--register-task")) return RegisterDriverTask() ? 0 : 1;
    // Used by the installer/uninstaller: close the running companion cleanly (it turns the phone camera
    // off on the way out) and wait for it to exit. Exit code 0 = none running or it closed.
    if (wcsstr(GetCommandLineW(), L"--quit")) return QuitRunningInstance();

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MyCamCompanion");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    LogInit();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFVirtualCamera> vcam;
    HRESULT vcamHr;
    if (IsWindows11OrGreater()) {
        vcamHr = CreateVirtualCamera(vcam);
        g_backendName = L"Frame Server";
    } else {
        vcamHr = IsDShowCameraRegistered() ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        g_backendName = L"DirectShow";
    }
    g_vcamOk = SUCCEEDED(vcamHr);
    Log("companion started; virtual camera (%ls) -> 0x%08X", g_backendName.c_str(), unsigned(vcamHr));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MyCamCompanion";
    RegisterClassW(&wc);
    // A hidden top-level window (never shown): unlike a message-only window it receives lock/unlock and
    // sleep notifications.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MyCam", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_hwnd;
    g_tray.uID = kTrayId;
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadTrayIcon(kIconDisconnected);
    g_trayIcon = kIconDisconnected;
    wcscpy_s(g_tray.szTip, L"MyCam");
    Shell_NotifyIconW(NIM_ADD, &g_tray);

    if (!g_vcamOk) {
        wchar_t msg[256];
        swprintf_s(msg, L"Could not register the MyCam virtual camera (0x%08X). Windows 11 is required, and "
                        L"install.ps1 must have been run once as administrator.", unsigned(vcamHr));
        Notify(L"MyCam", msg);
    }

    g_mirror = ReadSetting(L"Mirror", 0) != 0;
    g_fill = ReadSetting(L"Fill", 0) != 0;
    g_wireless = ReadSetting(L"Wireless", 0) != 0;
    PhoneLink link([](const LinkStatus& s) {
        {
            std::lock_guard<std::mutex> lock(g_statusLock);
            g_status = s;
        }
        PostMessageW(g_hwnd, WM_STATUS, 0, 0);
    }, [] { PostMessageW(g_hwnd, WM_NEED_DRIVER, 0, 0); });
    link.SetMirror(g_mirror);
    link.SetFill(g_fill);
    link.SetWireless(g_wireless);
    link.SetStatusImages(LoadStatusImage(kImagePaused), LoadStatusImage(kImageWaiting));
    g_link = &link;

    const bool testPattern = wcsstr(GetCommandLineW(), L"--test-pattern") != nullptr;
    PairingDialog pairing({CurrentStatus, [] { g_link->AllowPairing(); }, [] { g_link->CancelPairing(); }});
    g_pairing = &pairing;

    SettingsModel model;
    model.status = CurrentStatus;
    model.cameraRegistered = [] { return g_vcamOk; };
    model.backendName = [] { return g_backendName; };
    model.mirror = [] { return g_mirror; };
    model.setMirror = SetMirror;
    model.autostart = AutostartEnabled;
    model.setAutostart = SetAutostart;
    model.setFacing = [](int facing) { g_link->RequestFacing(facing); };
    model.reconnect = [] { g_link->RequestReconnect(); };
    model.openLogFolder = OpenLogFolder;
    model.paused = [] { return CurrentStatus().state == LinkState::Paused; };
    model.setPaused = [](bool pause) { g_link->RequestPause(pause); };
    model.fill = [] { return g_fill; };
    model.setFill = SetFill;
    model.command = [](uint8_t cmd, uint8_t arg) { g_link->RequestCommand(cmd, arg); };
    model.wireless = [] { return g_wireless; };
    model.setWireless = SetWireless;
    model.forgetPhones = ForgetPairedPhones; // They pair again (with a code) next time.
    model.showPairing = [] { if (g_pairing && g_settings) g_pairing->Show(g_settings->Hwnd()); };
    model.allowPairing = [] { g_link->AllowPairing(); };
    model.cancelPairing = [] { g_link->CancelPairing(); };
    model.scanForPhones = [] { g_link->ScanForPhones(); };
    model.connectTo = [](uint32_t ip) { g_link->ConnectTo(ip); };
    model.startWebPairing = [](uint32_t ip) { g_link->StartWebPairing(ip); };
    model.stopWebPairing = [] { g_link->StopWebPairing(); };
    model.forgetWebDevice = [](const std::string& id) { g_link->ForgetWebDevice(id); };
    model.testPattern = testPattern;
    LinkStatus demo;
    const bool demoMode = DemoStatus(&demo);
    if (demoMode && !testPattern) {
        // No phone link: Pause flips the made-up state, so status changes (and what Narrator announces
        // for them) can be checked too.
        model.setPaused = [](bool pause) {
            {
                std::lock_guard<std::mutex> lock(g_statusLock);
                if (g_status.state != LinkState::Streaming && g_status.state != LinkState::Paused) return;
                g_status.state = pause ? LinkState::Paused : LinkState::Streaming;
            }
            PostMessageW(g_hwnd, WM_STATUS, 0, 0);
        };
        model.scanForPhones = DemoScan;
        model.connectTo = [](uint32_t) { Log("demo: Connect pressed (no phone link in demo mode)"); };
    }
    SettingsWindow settings(std::move(model));
    g_settings = &settings;
    if (demoMode) {
        std::lock_guard<std::mutex> lock(g_statusLock);
        g_status = demo;
    }
    UpdateTray();
    if (wcsstr(GetCommandLineW(), L"--settings")) settings.Show();

    std::atomic<bool> quitPattern{false};
    std::thread worker([&] {
        if (demoMode && !testPattern) return; // Made-up status only: no phone link.
        if (testPattern) {
            const wchar_t* rot = wcsstr(GetCommandLineW(), L"--rotate=");
            RunTestPattern(quitPattern, rot ? uint32_t(_wtoi(rot + 9)) : 0);
            return;
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
        link.Run();
        MFShutdown();
        CoUninitialize();
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    link.Quit();
    quitPattern = true;
    worker.join();
    g_pairing = nullptr;
    g_settings = nullptr;
    g_link = nullptr;
    if (vcam) {
        vcam->Stop();
        vcam->Shutdown();
    }
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}

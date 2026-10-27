#include "winusb_bind.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#include <string>

namespace mycam {

// Phones in Android Open Accessory mode enumerate as Google 18d1:2d00..2d05 and Windows has no driver for them.
// With USB debugging on, the phone is a composite device instead (2d01, 2d04, 2d05: the accessory plus adb
// and/or audio). Windows' composite driver splits it into one instance per interface, and the accessory is
// always interface 0 (MI_00); the others (adb, audio) keep their own drivers. 2d02/2d03 are audio only.
bool IsAccessoryInstance(const std::wstring& id) {
    std::wstring upper = id;
    for (auto& c : upper) c = towupper(c);
    const std::wstring prefix = L"USB\\VID_18D1&PID_2D0";
    if (upper.rfind(prefix, 0) != 0 || upper.size() <= prefix.size()) return false;
    wchar_t pid = upper[prefix.size()];
    if (pid < L'0' || pid > L'5') return false;
    size_t mi = upper.find(L"&MI_");
    if (mi == std::wstring::npos) return true;
    return (pid == L'1' || pid == L'4' || pid == L'5') && upper.compare(mi, 6, L"&MI_00") == 0;
}

namespace {

bool NeedsDriver(DEVINST inst) {
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, inst, 0) != CR_SUCCESS) return false;
    return (status & DN_HAS_PROBLEM) && (problem == CM_PROB_FAILED_INSTALL || problem == CM_PROB_NOT_CONFIGURED);
}

std::wstring InstanceId(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    SetupDiGetDeviceInstanceIdW(set, dev, id, MAX_DEVICE_ID_LEN, nullptr);
    return id;
}

// Installs Windows' inbox "WinUsb Device" driver (winusb.inf) on one device, like picking it manually in
// Device Manager. The driver package is Microsoft-signed, so no custom signing is needed.
bool InstallWinUsb(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    wchar_t classGuid[] = L"{88BAE032-5A81-49f0-BC3D-A4FF138216D6}"; // USBDevice class (winusb.inf)
    if (!SetupDiSetDeviceRegistryPropertyW(set, dev, SPDRP_CLASSGUID, reinterpret_cast<BYTE*>(classGuid), sizeof(classGuid)))
        return false;
    dev->ClassGuid = {0x88bae032, 0x5a81, 0x49f0, {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}};

    SP_DEVINSTALL_PARAMS_W params = {sizeof(params)};
    if (!SetupDiGetDeviceInstallParamsW(set, dev, &params)) return false;
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    GetWindowsDirectoryW(params.DriverPath, MAX_PATH);
    wcscat_s(params.DriverPath, L"\\INF\\winusb.inf");
    if (!SetupDiSetDeviceInstallParamsW(set, dev, &params)) return false;
    if (!SetupDiBuildDriverInfoList(set, dev, SPDIT_CLASSDRIVER)) return false;

    SP_DRVINFO_DATA_W driver = {sizeof(driver)};
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDriverInfoW(set, dev, SPDIT_CLASSDRIVER, i, &driver); ++i) {
        if (wcsstr(driver.Description, L"WinUsb")) { found = true; break; }
    }
    if (!found || !SetupDiSetSelectedDriverW(set, dev, &driver)) return false;

    BOOL reboot = FALSE;
    return DiInstallDevice(nullptr, set, dev, &driver, 0, &reboot) != FALSE;
}

} // namespace

bool AccessoryNeedsDriver() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool needs = false;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; !needs && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        needs = IsAccessoryInstance(InstanceId(set, &dev)) && NeedsDriver(dev.DevInst);
    }
    SetupDiDestroyDeviceInfoList(set);
    return needs;
}

int BindAccessoryDrivers() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int bound = 0;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        if (IsAccessoryInstance(InstanceId(set, &dev)) && NeedsDriver(dev.DevInst) && InstallWinUsb(set, &dev)) ++bound;
    }
    SetupDiDestroyDeviceInfoList(set);
    return bound;
}

} // namespace mycam

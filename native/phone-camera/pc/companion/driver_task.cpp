#include "driver_task.h"

#include <windows.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <string>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace mycam {

namespace {

constexpr wchar_t kTaskName[] = L"MyCam phone driver";

// Administrators and SYSTEM: full control. Authenticated users: read and run only.
constexpr wchar_t kTaskSddl[] = L"D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;AU)";

struct Bstr {
    BSTR s;
    explicit Bstr(const wchar_t* text) : s(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(s); }
    operator BSTR() const { return s; }
};

VARIANT Empty() { VARIANT v; VariantInit(&v); return v; }

VARIANT String(BSTR s) {
    VARIANT v; VariantInit(&v);
    v.vt = VT_BSTR; v.bstrVal = s; // Borrowed: the caller keeps the Bstr alive.
    return v;
}

// COM for the current thread, if it isn't initialised already.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

ComPtr<ITaskFolder> RootFolder() {
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) return nullptr;
    if (FAILED(service->Connect(Empty(), Empty(), Empty(), Empty()))) return nullptr;
    ComPtr<ITaskFolder> root;
    if (FAILED(service->GetFolder(Bstr(L"\\"), &root))) return nullptr;
    return root;
}

} // namespace

bool RegisterDriverTask() {
    ComScope com;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (SUCCEEDED(hr)) hr = service->Connect(Empty(), Empty(), Empty(), Empty());
    ComPtr<ITaskFolder> root;
    if (SUCCEEDED(hr)) hr = service->GetFolder(Bstr(L"\\"), &root);
    ComPtr<ITaskDefinition> task;
    if (SUCCEEDED(hr)) hr = service->NewTask(0, &task);
    if (FAILED(hr)) { Log("driver task: setup failed 0x%08X", unsigned(hr)); return false; }

    ComPtr<IRegistrationInfo> info;
    if (SUCCEEDED(task->get_RegistrationInfo(&info))) {
        info->put_Author(Bstr(L"MyCam"));
        info->put_Description(Bstr(L"Installs Windows' WinUSB driver for a phone in MyCam's USB accessory mode, "
                                   L"so new phones work without an admin prompt."));
    }

    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(task->get_Principal(&principal))) {
        principal->put_UserId(Bstr(L"S-1-5-18")); // SYSTEM
        principal->put_LogonType(TASK_LOGON_SERVICE_ACCOUNT);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }

    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(task->get_Settings(&settings))) {
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(Bstr(L"PT2M"));
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_StartWhenAvailable(VARIANT_FALSE);
    }

    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    hr = task->get_Actions(&actions);
    if (SUCCEEDED(hr)) hr = actions->Create(TASK_ACTION_EXEC, &action);
    if (SUCCEEDED(hr)) hr = action.As(&exec);
    if (SUCCEEDED(hr)) hr = exec->put_Path(Bstr(exe));
    if (SUCCEEDED(hr)) hr = exec->put_Arguments(Bstr(L"--bind-driver"));
    if (FAILED(hr)) { Log("driver task: action failed 0x%08X", unsigned(hr)); return false; }

    Bstr sddl(kTaskSddl);
    ComPtr<IRegisteredTask> registered;
    hr = root->RegisterTaskDefinition(Bstr(kTaskName), task.Get(), TASK_CREATE_OR_UPDATE, Empty(), Empty(),
                                      TASK_LOGON_SERVICE_ACCOUNT, String(sddl), &registered);
    Log("driver task: register -> 0x%08X", unsigned(hr));
    return SUCCEEDED(hr);
}

bool RunDriverTask() {
    ComScope com;
    ComPtr<ITaskFolder> root = RootFolder();
    if (!root) return false;
    ComPtr<IRegisteredTask> task;
    HRESULT hr = root->GetTask(Bstr(kTaskName), &task);
    if (FAILED(hr)) { Log("driver task: not found (0x%08X)", unsigned(hr)); return false; }

    // Only trust a task that still runs our own exe (an admin could have edited it; never run anything else).
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ComPtr<ITaskDefinition> def;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    BSTR path = nullptr;
    if (FAILED(task->get_Definition(&def)) || FAILED(def->get_Actions(&actions)) ||
        FAILED(actions->get_Item(1, &action)) || FAILED(action.As(&exec)) || FAILED(exec->get_Path(&path))) {
        return false;
    }
    bool ours = path && _wcsicmp(path, exe) == 0;
    SysFreeString(path);
    if (!ours) { Log("driver task: points at another exe, not using it"); return false; }

    ComPtr<IRunningTask> running;
    hr = task->Run(Empty(), &running);
    Log("driver task: run -> 0x%08X", unsigned(hr));
    return SUCCEEDED(hr);
}

} // namespace mycam

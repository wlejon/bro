// app_schedule.h on Windows: Task Scheduler 2.0 (ITaskService). Each launch
// is a task in the root folder registered for the current user with an
// interactive token (runs only while the user is logged on, in their
// session, no password and no elevation), one time trigger, and "run as soon
// as possible after a missed start".
#include "platform/app_schedule.h"

#include "util/process_spawn.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <cstdio>
#include <ctime>

namespace bro::platform::schedule {

namespace {

using Microsoft::WRL::ComPtr;

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string hresultText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    std::string s = buf;
    wchar_t msg[512] = {};
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0,
                       msg, 512, nullptr)) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            std::string t(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, msg, -1, t.data(), n, nullptr, nullptr);
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == '.')) t.pop_back();
            s += " (" + t + ")";
        }
    }
    return s;
}

// COM for the call's length, whatever this thread already chose.
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(hr);  // S_FALSE too: balanced by CoUninitialize
    }
    ~ComScope() {
        if (owned) CoUninitialize();
    }
};

struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const std::wstring& s) : b(SysAllocString(s.c_str())) {}
    ~Bstr() { SysFreeString(b); }
    operator BSTR() const { return b; }
};

bool connect(ComPtr<ITaskService>& svc, ComPtr<ITaskFolder>& folder, std::string& why) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
    if (FAILED(hr)) {
        why = "Task Scheduler is not available: " + hresultText(hr);
        return false;
    }
    VARIANT empty;
    VariantInit(&empty);
    hr = svc->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        why = "connecting to Task Scheduler failed: " + hresultText(hr);
        return false;
    }
    hr = svc->GetFolder(Bstr(L"\\"), &folder);
    if (FAILED(hr)) {
        why = "Task Scheduler's root folder: " + hresultText(hr);
        return false;
    }
    return true;
}

// Local time as Task Scheduler writes it, "2026-11-04T07:30:05".
std::wstring boundary(int64_t whenMs) {
    std::time_t t = static_cast<std::time_t>((whenMs + 999) / 1000);  // never before `when`
    std::tm lt{};
    localtime_s(&lt, &t);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02dT%02d:%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
             lt.tm_min, lt.tm_sec);
    return buf;
}

}  // namespace

std::string backend(std::string*) { return "task-scheduler"; }

bool add(const Launch& launch, std::string& why) {
    if (launch.command.empty()) {
        why = "no command";
        return false;
    }
    ComScope com;
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> folder;
    if (!connect(svc, folder, why)) return false;
    HRESULT hr;
    ComPtr<ITaskDefinition> def;
    if (FAILED(hr = svc->NewTask(0, &def))) {
        why = "NewTask: " + hresultText(hr);
        return false;
    }
    {
        ComPtr<IRegistrationInfo> info;
        if (SUCCEEDED(def->get_RegistrationInfo(&info))) {
            info->put_Author(Bstr(L"bro"));
            info->put_Description(Bstr(wide(launch.description)));
        }
    }
    {
        ComPtr<IPrincipal> principal;
        if (SUCCEEDED(def->get_Principal(&principal))) {
            principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            principal->put_RunLevel(TASK_RUNLEVEL_LUA);
        }
    }
    {
        ComPtr<ITaskSettings> settings;
        if (SUCCEEDED(def->get_Settings(&settings))) {
            settings->put_StartWhenAvailable(VARIANT_TRUE);
            settings->put_WakeToRun(launch.wake ? VARIANT_TRUE : VARIANT_FALSE);
            settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));  // the app runs as long as it likes
            settings->put_MultipleInstances(TASK_INSTANCES_PARALLEL);
            settings->put_Enabled(VARIANT_TRUE);
        }
    }
    {
        ComPtr<ITriggerCollection> triggers;
        ComPtr<ITrigger> trigger;
        if (FAILED(hr = def->get_Triggers(&triggers)) || FAILED(hr = triggers->Create(TASK_TRIGGER_TIME, &trigger))) {
            why = "creating the time trigger: " + hresultText(hr);
            return false;
        }
        ComPtr<ITimeTrigger> time;
        trigger.As(&time);
        if (!time || FAILED(hr = time->put_StartBoundary(Bstr(boundary(launch.whenMs))))) {
            why = "setting the trigger's time: " + hresultText(hr);
            return false;
        }
        time->put_Enabled(VARIANT_TRUE);
    }
    {
        ComPtr<IActionCollection> actions;
        ComPtr<IAction> action;
        if (FAILED(hr = def->get_Actions(&actions)) || FAILED(hr = actions->Create(TASK_ACTION_EXEC, &action))) {
            why = "creating the action: " + hresultText(hr);
            return false;
        }
        ComPtr<IExecAction> exec;
        action.As(&exec);
        if (!exec) {
            why = "the action is not an exec action";
            return false;
        }
        exec->put_Path(Bstr(wide(launch.command[0])));
        std::vector<std::string> rest(launch.command.begin() + 1, launch.command.end());
        if (!rest.empty()) exec->put_Arguments(Bstr(util::windowsCommandLine(rest)));
        if (!launch.cwd.empty()) exec->put_WorkingDirectory(Bstr(wide(launch.cwd)));
    }
    ComPtr<IRegisteredTask> registered;
    VARIANT none;
    VariantInit(&none);
    hr = folder->RegisterTaskDefinition(Bstr(wide(launch.name)), def.Get(), TASK_CREATE_OR_UPDATE, none, none,
                                        TASK_LOGON_INTERACTIVE_TOKEN, none, &registered);
    if (FAILED(hr)) {
        why = "registering the task: " + hresultText(hr);
        return false;
    }
    return true;
}

bool remove(const std::string& name, std::string* why) {
    ComScope com;
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> folder;
    std::string err;
    if (!connect(svc, folder, err)) {
        if (why) *why = err;
        return false;
    }
    const HRESULT hr = folder->DeleteTask(Bstr(wide(name)), 0);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
    if (why) *why = "deleting the task: " + hresultText(hr);
    return false;
}

bool exists(const std::string& name) {
    ComScope com;
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> folder;
    std::string err;
    if (!connect(svc, folder, err)) return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(folder->GetTask(Bstr(wide(name)), &task));
}

}  // namespace bro::platform::schedule

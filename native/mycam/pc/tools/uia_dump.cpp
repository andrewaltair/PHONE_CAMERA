// Developer tool: checks the companion's UI Automation tree the way a screen reader sees it.
//
//   uia_dump                      dump the MyCam settings window
//   uia_dump --pairing            dump the pairing dialog instead
//   uia_dump --listen 3000 --toggle Mirror --expand GroupVideo --select Quality720p --invoke ShowPairing
//                                 subscribe to UIA events, run the actions in order (by AutomationId),
//                                 print the events that arrive, then dump the tree again
//   Other actions: --collapse ID, --focus ID. --wait MS sets the pause after each action (default 400).
//   --no-dump skips the final dump.

#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_print;

std::wstring Bstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
    SysFreeString(b);
    return s;
}

const wchar_t* TypeName(CONTROLTYPEID t) {
    switch (t) {
    case UIA_WindowControlTypeId: return L"Window";
    case UIA_PaneControlTypeId: return L"Pane";
    case UIA_GroupControlTypeId: return L"Group";
    case UIA_ButtonControlTypeId: return L"Button";
    case UIA_CheckBoxControlTypeId: return L"CheckBox";
    case UIA_RadioButtonControlTypeId: return L"RadioButton";
    case UIA_HyperlinkControlTypeId: return L"Hyperlink";
    case UIA_TextControlTypeId: return L"Text";
    case UIA_ImageControlTypeId: return L"Image";
    case UIA_ProgressBarControlTypeId: return L"ProgressBar";
    case UIA_TitleBarControlTypeId: return L"TitleBar";
    case UIA_MenuBarControlTypeId: return L"MenuBar";
    }
    return L"Other";
}

std::wstring Describe(IUIAutomationElement* e) {
    CONTROLTYPEID type = 0;
    e->get_CurrentControlType(&type);
    BSTR b = nullptr;
    e->get_CurrentName(&b);
    const std::wstring name = Bstr(b);
    e->get_CurrentAutomationId(&b);
    const std::wstring aid = Bstr(b);
    std::wstring s = std::wstring(TypeName(type)) + L" \"" + name + L"\"";
    if (!aid.empty()) s += L" #" + aid;

    BOOL v = FALSE;
    std::wstring states;
    if (SUCCEEDED(e->get_CurrentIsEnabled(&v)) && !v) states += L" disabled";
    if (SUCCEEDED(e->get_CurrentIsKeyboardFocusable(&v)) && v) states += L" focusable";
    if (SUCCEEDED(e->get_CurrentHasKeyboardFocus(&v)) && v) states += L" FOCUSED";
    if (SUCCEEDED(e->get_CurrentIsOffscreen(&v)) && v) states += L" offscreen";
    e->get_CurrentAccessKey(&b);
    const std::wstring key = Bstr(b);
    if (!key.empty()) states += L" key=" + key;
    e->get_CurrentItemStatus(&b);
    const std::wstring status = Bstr(b);
    if (!status.empty()) states += L" status=" + status;
    VARIANT var;
    VariantInit(&var);
    if (SUCCEEDED(e->GetCurrentPropertyValue(UIA_LiveSettingPropertyId, &var)) && var.vt == VT_I4 && var.lVal != Off)
        states += var.lVal == Polite ? L" live=polite" : L" live=assertive";
    VariantClear(&var);

    std::wstring patterns;
    IUnknown* p = nullptr;
    if (SUCCEEDED(e->GetCurrentPattern(UIA_InvokePatternId, &p)) && p) { patterns += L" Invoke"; p->Release(); p = nullptr; }
    IUIAutomationTogglePattern* toggle = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) && toggle) {
        ToggleState t = ToggleState_Off;
        toggle->get_CurrentToggleState(&t);
        patterns += t == ToggleState_On ? L" Toggle(on)" : L" Toggle(off)";
        toggle->Release();
    }
    IUIAutomationSelectionItemPattern* item = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&item))) && item) {
        item->get_CurrentIsSelected(&v);
        patterns += v ? L" SelectionItem(selected)" : L" SelectionItem";
        item->Release();
    }
    IUIAutomationSelectionPattern* sel = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&sel))) && sel) {
        patterns += L" Selection";
        IUIAutomationElementArray* picked = nullptr;
        if (SUCCEEDED(sel->GetCurrentSelection(&picked)) && picked) {
            int n = 0;
            picked->get_Length(&n);
            for (int i = 0; i < n; ++i) {
                IUIAutomationElement* x = nullptr;
                if (SUCCEEDED(picked->GetElement(i, &x)) && x) {
                    x->get_CurrentName(&b);
                    patterns += L"[" + Bstr(b) + L"]";
                    x->Release();
                }
            }
            picked->Release();
        }
        sel->Release();
    }
    IUIAutomationExpandCollapsePattern* ec = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&ec))) && ec) {
        ExpandCollapseState st = ExpandCollapseState_LeafNode;
        ec->get_CurrentExpandCollapseState(&st);
        patterns += st == ExpandCollapseState_Expanded ? L" ExpandCollapse(expanded)" : L" ExpandCollapse(collapsed)";
        ec->Release();
    }
    IUIAutomationRangeValuePattern* range = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range))) && range) {
        double val = 0, max = 0;
        range->get_CurrentValue(&val);
        range->get_CurrentMaximum(&max);
        wchar_t buf[64];
        swprintf_s(buf, L" RangeValue(%.0f/%.0f)", val, max);
        patterns += buf;
        range->Release();
    }
    RECT r = {};
    e->get_CurrentBoundingRectangle(&r);
    wchar_t rect[80];
    swprintf_s(rect, L"  @%ld,%ld %ldx%ld", r.left, r.top, r.right - r.left, r.bottom - r.top);
    if (!states.empty()) s += L" {" + states.substr(1) + L"}";
    if (!patterns.empty()) s += L" <" + patterns.substr(1) + L">";
    return s + rect;
}

void Dump(IUIAutomationTreeWalker* walker, IUIAutomationElement* e, int depth) {
    wprintf(L"%*s%s\n", depth * 2, L"", Describe(e).c_str());
    IUIAutomationElement* child = nullptr;
    walker->GetFirstChildElement(e, &child);
    while (child) {
        Dump(walker, child, depth + 1);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
}

// Prints every UIA event the companion raises.
class Events final : public IUIAutomationEventHandler,
                     public IUIAutomationFocusChangedEventHandler,
                     public IUIAutomationPropertyChangedEventHandler,
                     public IUIAutomationStructureChangedEventHandler {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomationEventHandler)) *out = static_cast<IUIAutomationEventHandler*>(this);
        else if (riid == __uuidof(IUIAutomationFocusChangedEventHandler)) *out = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
        else if (riid == __uuidof(IUIAutomationPropertyChangedEventHandler)) *out = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
        else if (riid == __uuidof(IUIAutomationStructureChangedEventHandler)) *out = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return n;
    }
    IFACEMETHODIMP HandleAutomationEvent(IUIAutomationElement* e, EVENTID id) override {
        const wchar_t* what = id == UIA_LiveRegionChangedEventId ? L"LiveRegionChanged"
                            : id == UIA_SelectionItem_ElementSelectedEventId ? L"ElementSelected" : L"Event";
        Print(what, e, L"");
        return S_OK;
    }
    IFACEMETHODIMP HandleFocusChangedEvent(IUIAutomationElement* e) override {
        Print(L"FocusChanged", e, L"");
        return S_OK;
    }
    IFACEMETHODIMP HandlePropertyChangedEvent(IUIAutomationElement* e, PROPERTYID id, VARIANT v) override {
        wchar_t buf[300];
        const wchar_t* prop = id == UIA_ToggleToggleStatePropertyId ? L"ToggleState"
                            : id == UIA_SelectionItemIsSelectedPropertyId ? L"IsSelected"
                            : id == UIA_ExpandCollapseExpandCollapseStatePropertyId ? L"ExpandCollapseState"
                            : id == UIA_NamePropertyId ? L"Name" : id == UIA_IsEnabledPropertyId ? L"IsEnabled" : L"Property";
        if (v.vt == VT_BSTR) swprintf_s(buf, L"%s = \"%s\"", prop, v.bstrVal ? v.bstrVal : L"");
        else if (v.vt == VT_BOOL) swprintf_s(buf, L"%s = %s", prop, v.boolVal ? L"true" : L"false");
        else if (v.vt == VT_I4) swprintf_s(buf, L"%s = %ld", prop, v.lVal);
        else swprintf_s(buf, L"%s (vt %d)", prop, int(v.vt));
        Print(L"PropertyChanged", e, buf);
        return S_OK;
    }
    IFACEMETHODIMP HandleStructureChangedEvent(IUIAutomationElement* e, StructureChangeType type, SAFEARRAY*) override {
        Print(type == StructureChangeType_ChildrenInvalidated ? L"StructureChanged(ChildrenInvalidated)" : L"StructureChanged", e, L"");
        return S_OK;
    }

private:
    static void Print(const wchar_t* what, IUIAutomationElement* e, const wchar_t* extra) {
        BSTR b = nullptr;
        std::wstring name, aid;
        if (e) {
            e->get_CachedName(&b);
            if (!b) e->get_CurrentName(&b);
            name = Bstr(b);
            b = nullptr;
            e->get_CurrentAutomationId(&b);
            aid = Bstr(b);
        }
        std::lock_guard<std::mutex> lock(g_print);
        wprintf(L"  [event] %s: \"%s\" #%s %s\n", what, name.c_str(), aid.c_str(), extra);
        fflush(stdout);
    }
    LONG refs_ = 1;
};

IUIAutomationElement* FindById(IUIAutomation* uia, IUIAutomationElement* root, const wchar_t* id) {
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(id);
    IUIAutomationCondition* cond = nullptr;
    uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond);
    VariantClear(&v);
    IUIAutomationElement* found = nullptr;
    if (cond) {
        root->FindFirst(TreeScope_Descendants, cond, &found);
        cond->Release();
    }
    return found;
}

HRESULT RunAction(IUIAutomation* uia, IUIAutomationElement* root, const std::wstring& action, const wchar_t* id) {
    IUIAutomationElement* e = FindById(uia, root, id);
    if (!e) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    HRESULT hr = E_NOINTERFACE;
    if (action == L"--invoke") {
        IUIAutomationInvokePattern* p = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&p))) && p) { hr = p->Invoke(); p->Release(); }
    } else if (action == L"--toggle") {
        IUIAutomationTogglePattern* p = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&p))) && p) { hr = p->Toggle(); p->Release(); }
    } else if (action == L"--select") {
        IUIAutomationSelectionItemPattern* p = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&p))) && p) { hr = p->Select(); p->Release(); }
    } else if (action == L"--expand" || action == L"--collapse") {
        IUIAutomationExpandCollapsePattern* p = nullptr;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&p))) && p) {
            hr = action == L"--expand" ? p->Expand() : p->Collapse();
            p->Release();
        }
    } else if (action == L"--focus") {
        hr = e->SetFocus();
    }
    e->Release();
    return hr;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* cls = L"MyCamSettings";
    int listenMs = 0, waitMs = 400;
    bool dump = true;
    std::vector<std::pair<std::wstring, std::wstring>> actions;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--pairing") cls = L"MyCamPairing";
        else if (a == L"--no-dump") dump = false;
        else if (a == L"--listen" && i + 1 < argc) listenMs = _wtoi(argv[++i]);
        else if (a == L"--wait" && i + 1 < argc) waitMs = _wtoi(argv[++i]);
        else if ((a == L"--invoke" || a == L"--toggle" || a == L"--select" || a == L"--expand" || a == L"--collapse" ||
                  a == L"--focus") && i + 1 < argc) actions.push_back({a, argv[++i]});
        else {
            fwprintf(stderr, L"unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    _setmode(_fileno(stdout), _O_U8TEXT); // Names contain ·, × and quotes.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
        fwprintf(stderr, L"UI Automation unavailable\n");
        return 1;
    }
    HWND hwnd = FindWindowW(cls, nullptr);
    if (!hwnd) {
        fwprintf(stderr, L"no %s window (run MyCamCompanion.exe --settings)\n", cls);
        return 1;
    }
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) {
        fwprintf(stderr, L"ElementFromHandle failed\n");
        return 1;
    }

    Events* events = nullptr;
    if (listenMs > 0) {
        events = new Events();
        uia->AddAutomationEventHandler(UIA_LiveRegionChangedEventId, root, TreeScope_Subtree, nullptr, events);
        uia->AddAutomationEventHandler(UIA_SelectionItem_ElementSelectedEventId, root, TreeScope_Subtree, nullptr, events);
        PROPERTYID props[] = {UIA_ToggleToggleStatePropertyId, UIA_SelectionItemIsSelectedPropertyId,
                              UIA_ExpandCollapseExpandCollapseStatePropertyId, UIA_NamePropertyId, UIA_IsEnabledPropertyId};
        uia->AddPropertyChangedEventHandlerNativeArray(root, TreeScope_Subtree, nullptr, events, props, int(std::size(props)));
        uia->AddStructureChangedEventHandler(root, TreeScope_Subtree, nullptr, events);
        uia->AddFocusChangedEventHandler(nullptr, events);
        wprintf(L"listening for events\n");
    }

    for (const auto& [action, id] : actions) {
        const HRESULT hr = RunAction(uia, root, action, id.c_str());
        {
            std::lock_guard<std::mutex> lock(g_print);
            wprintf(L"%s %s -> 0x%08X\n", action.c_str(), id.c_str(), unsigned(hr));
            fflush(stdout);
        }
        Sleep(DWORD(waitMs));
    }
    if (listenMs > 0) Sleep(DWORD(listenMs));

    if (dump) {
        IUIAutomationTreeWalker* walker = nullptr;
        uia->get_RawViewWalker(&walker);
        std::lock_guard<std::mutex> lock(g_print);
        if (IsWindow(hwnd)) Dump(walker, root, 0);
        else wprintf(L"(window closed)\n");
        walker->Release();
    }
    if (events) {
        uia->RemoveAllEventHandlers();
        events->Release();
    }
    root->Release();
    uia->Release();
    CoUninitialize();
    return 0;
}

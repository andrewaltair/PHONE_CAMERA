#include "uia_provider.h"

#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace mycam {

using ui::Element;
using ui::Kind;

namespace {

constexpr int kRoot = ui::kNone; // Fragment id 0 is the window's root fragment.
constexpr int kSetNodeBase = -1000; // Synthetic radio-set containers: kSetNodeBase - radioSet.

int SetNodeId(int radioSet) { return kSetNodeBase - radioSet; }
bool IsSetNode(int id) { return id < 0; }

enum Action : WPARAM { kActInvoke = 1, kActFocus, kActSelect, kActExpand, kActCollapse };

const wchar_t* SetName(int set) {
    switch (set) {
    case ui::kSetFacing: return L"Camera";
    case ui::kSetQuality: return L"Quality";
    case ui::kSetFps: return L"Frame rate";
    case ui::kSetFocus: return L"Focus";
    }
    return L"Options";
}

ui::Box Union(const ui::Box& a, const ui::Box& b) {
    return {std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r), std::max(a.b, b.b)};
}

ui::Box Intersect(const ui::Box& a, const ui::Box& b) {
    return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)};
}

class Fragment;

} // namespace

struct UiaHost::State {
    std::mutex mutex;
    HWND hwnd = nullptr;
    UINT actionMsg = 0;
    bool alive = true;
    bool requested = false;          // A client asked for the provider (WM_GETOBJECT): keep the snapshot fresh.
    // Snapshot (visible elements only, synthetic radio-set containers inserted, parents normalised so that
    // every parent is in the list or kRoot).
    std::vector<Element> elements;
    std::vector<RECT> screen;        // Screen pixels, parallel to `elements`, as of `origin`.
    POINT origin = {};               // Client (0,0) in screen pixels when the snapshot was taken.
    int focused = ui::kNone;
    bool windowFocused = false;
    std::map<int, Fragment*> cache;  // One provider per id (owns a reference), for identity and disconnecting.

    const Element* Find(int id) const {
        for (const auto& e : elements) if (e.id == id) return &e;
        return nullptr;
    }
    int IndexOf(int id) const {
        for (size_t i = 0; i < elements.size(); ++i) if (elements[i].id == id) return int(i);
        return -1;
    }
};

namespace {

using State = UiaHost::State;

class Fragment final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IInvokeProvider,
                       public IToggleProvider,
                       public ISelectionItemProvider,
                       public ISelectionProvider,
                       public IExpandCollapseProvider,
                       public IRangeValueProvider {
public:
    Fragment(std::shared_ptr<State> state, int id) : state_(std::move(state)), id_(id) {}

    // Returns the cached provider for `id` (AddRef'd), creating it. Caller holds state->mutex.
    static Fragment* Get(const std::shared_ptr<State>& state, int id) {
        auto it = state->cache.find(id);
        Fragment* f;
        if (it != state->cache.end()) {
            f = it->second;
        } else {
            f = new Fragment(state, id); // The cache owns this first reference.
            state->cache[id] = f;
        }
        f->AddRef();
        return f;
    }

    // --- IUnknown --------------------------------------------------------------------------------------
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot) && id_ == kRoot) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (riid == __uuidof(IInvokeProvider)) *out = static_cast<IInvokeProvider*>(this);
        else if (riid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
        else if (riid == __uuidof(ISelectionItemProvider)) *out = static_cast<ISelectionItemProvider*>(this);
        else if (riid == __uuidof(ISelectionProvider)) *out = static_cast<ISelectionProvider*>(this);
        else if (riid == __uuidof(IExpandCollapseProvider)) *out = static_cast<IExpandCollapseProvider*>(this);
        else if (riid == __uuidof(IRangeValueProvider)) *out = static_cast<IRangeValueProvider*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // --- IRawElementProviderSimple ---------------------------------------------------------------------
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override {
        if (!out) return E_POINTER;
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id_ == kRoot) return S_OK;
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        bool has = false;
        IID iid = IID_NULL;
        switch (pattern) {
        case UIA_InvokePatternId:
            has = e.kind == Kind::Button || e.kind == Kind::CaptionButton || e.kind == Kind::Link;
            iid = __uuidof(IInvokeProvider);
            break;
        case UIA_TogglePatternId: has = e.kind == Kind::Checkbox; iid = __uuidof(IToggleProvider); break;
        case UIA_SelectionItemPatternId: has = e.kind == Kind::Radio; iid = __uuidof(ISelectionItemProvider); break;
        case UIA_SelectionPatternId: has = IsSetNode(id_); iid = __uuidof(ISelectionProvider); break;
        case UIA_ExpandCollapsePatternId:
            has = e.kind == Kind::Group && e.expandable && !IsSetNode(id_);
            iid = __uuidof(IExpandCollapseProvider);
            break;
        case UIA_RangeValuePatternId: has = e.kind == Kind::ProgressBar && e.value >= 0; iid = __uuidof(IRangeValueProvider); break;
        }
        if (has) return QueryInterface(iid, reinterpret_cast<void**>(out));
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID prop, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        if (id_ == kRoot) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
            if (prop == UIA_ProviderDescriptionPropertyId) SetString(out, L"MyCam companion");
            return S_OK; // Name, control type, ... come from the window (host provider).
        }
        Element e;
        int focused = 0, posInSet = 0, setSize = 0;
        bool windowFocused = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
            const Element* p = state_->Find(id_);
            if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
            e = *p;
            focused = state_->focused;
            windowFocused = state_->windowFocused;
            if (e.kind == Kind::Radio) {
                for (const auto& o : state_->elements) {
                    if (o.kind != Kind::Radio || o.radioSet != e.radioSet) continue;
                    ++setSize;
                    if (o.id == id_) posInSet = setSize;
                }
            }
        }
        switch (prop) {
        case UIA_ControlTypePropertyId: {
            long type = UIA_TextControlTypeId;
            switch (e.kind) {
            case Kind::Group: type = UIA_GroupControlTypeId; break;
            case Kind::Button: case Kind::CaptionButton: type = UIA_ButtonControlTypeId; break;
            case Kind::Checkbox: type = UIA_CheckBoxControlTypeId; break;
            case Kind::Radio: type = UIA_RadioButtonControlTypeId; break;
            case Kind::Link: type = UIA_HyperlinkControlTypeId; break;
            case Kind::Text: type = UIA_TextControlTypeId; break;
            case Kind::Image: type = UIA_ImageControlTypeId; break;
            case Kind::ProgressBar: type = UIA_ProgressBarControlTypeId; break;
            }
            out->vt = VT_I4;
            out->lVal = type;
            break;
        }
        case UIA_NamePropertyId: SetString(out, e.name.c_str()); break;
        case UIA_AutomationIdPropertyId: SetString(out, UiaAutomationId(id_).c_str()); break;
        case UIA_AccessKeyPropertyId:
            if (e.accessKeyIndex >= 0 && e.accessKeyIndex < int(e.name.size())) {
                wchar_t key = e.name[size_t(e.accessKeyIndex)];
                if (key >= L'a' && key <= L'z') key = wchar_t(key - L'a' + L'A');
                SetString(out, (std::wstring(L"Alt+") + key).c_str());
            }
            break;
        case UIA_IsEnabledPropertyId: SetBool(out, e.enabled); break;
        case UIA_IsKeyboardFocusablePropertyId: SetBool(out, ui::CanFocus(e)); break;
        case UIA_HasKeyboardFocusPropertyId: SetBool(out, windowFocused && focused == id_); break;
        case UIA_IsOffscreenPropertyId: SetBool(out, e.Offscreen()); break;
        case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId: SetBool(out, true); break;
        case UIA_LiveSettingPropertyId:
            if (e.liveRegion) {
                out->vt = VT_I4;
                out->lVal = Polite;
            }
            break;
        case UIA_ItemStatusPropertyId:
            if (e.kind == Kind::ProgressBar && e.value < 0) SetString(out, L"Working");
            break;
        case UIA_PositionInSetPropertyId:
            if (posInSet) { out->vt = VT_I4; out->lVal = posInSet; }
            break;
        case UIA_SizeOfSetPropertyId:
            if (setSize) { out->vt = VT_I4; out->lVal = setSize; }
            break;
        case UIA_FrameworkIdPropertyId: SetString(out, L"MyCam"); break;
        }
        return S_OK;
    }

    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id_ != kRoot) return S_OK;
        HWND hwnd;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
            hwnd = state_->hwnd;
        }
        return UiaHostProviderFromHwnd(hwnd, out);
    }

    // --- IRawElementProviderFragment -------------------------------------------------------------------
    IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& list = state_->elements;
        int target = INT_MIN;
        if (id_ == kRoot) {
            if (dir == NavigateDirection_FirstChild || dir == NavigateDirection_LastChild) target = Child(kRoot, dir == NavigateDirection_LastChild);
        } else {
            const int at = state_->IndexOf(id_);
            if (at < 0) return UIA_E_ELEMENTNOTAVAILABLE;
            const int parent = list[size_t(at)].parent;
            switch (dir) {
            case NavigateDirection_Parent: target = parent; break;
            case NavigateDirection_NextSibling:
                for (size_t i = size_t(at) + 1; i < list.size(); ++i) if (list[i].parent == parent) { target = list[i].id; break; }
                break;
            case NavigateDirection_PreviousSibling:
                for (int i = at - 1; i >= 0; --i) if (list[size_t(i)].parent == parent) { target = list[size_t(i)].id; break; }
                break;
            case NavigateDirection_FirstChild: target = Child(id_, false); break;
            case NavigateDirection_LastChild: target = Child(id_, true); break;
            }
        }
        if (target != INT_MIN) *out = static_cast<IRawElementProviderFragment*>(Get(state_, target));
        return S_OK;
    }

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id_ == kRoot) return S_OK; // UIA builds the root's id from the window.
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive || !state_->Find(id_)) return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int rid[2] = {UiaAppendRuntimeId, id_};
        SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!sa) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(sa, &i, &rid[i]);
        *out = sa;
        return S_OK;
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
        if (!out) return E_POINTER;
        *out = {};
        if (id_ == kRoot) return S_OK; // The window's rectangle comes from the host provider.
        RECT r;
        POINT origin;
        HWND hwnd;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
            const int at = state_->IndexOf(id_);
            if (at < 0) return UIA_E_ELEMENTNOTAVAILABLE;
            r = state_->screen[size_t(at)];
            origin = state_->origin;
            hwnd = state_->hwnd;
        }
        // The window may have moved since the snapshot: follow it.
        POINT now = {0, 0};
        if (ClientToScreen(hwnd, &now)) OffsetRect(&r, now.x - origin.x, now.y - origin.y);
        out->left = r.left;
        out->top = r.top;
        out->width = r.right - r.left;
        out->height = r.bottom - r.top;
        return S_OK;
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override {
        if (id_ == kRoot) return S_OK;
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        if (!ui::CanFocus(e)) return UIA_E_INVALIDOPERATION;
        return Post(kActFocus);
    }

    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        *out = static_cast<IRawElementProviderFragmentRoot*>(Get(state_, kRoot));
        return S_OK;
    }

    // --- IRawElementProviderFragmentRoot (root only) -----------------------------------------------------
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        HWND hwnd;
        POINT origin;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
            hwnd = state_->hwnd;
            origin = state_->origin;
        }
        POINT now = {0, 0};
        if (ClientToScreen(hwnd, &now)) {
            x -= now.x - origin.x;
            y -= now.y - origin.y;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        int best = INT_MIN;
        double bestArea = 0;
        for (size_t i = 0; i < state_->elements.size(); ++i) {
            const Element& e = state_->elements[i];
            const RECT& r = state_->screen[i];
            if (e.Offscreen() || x < r.left || x >= r.right || y < r.top || y >= r.bottom) continue;
            const double area = double(r.right - r.left) * double(r.bottom - r.top);
            if (best == INT_MIN || area < bestArea) {
                best = e.id;
                bestArea = area;
            }
        }
        if (best != INT_MIN) *out = static_cast<IRawElementProviderFragment*>(Get(state_, best));
        return S_OK;
    }

    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        if (state_->focused != ui::kNone && state_->Find(state_->focused))
            *out = static_cast<IRawElementProviderFragment*>(Get(state_, state_->focused));
        return S_OK;
    }

    // --- IInvokeProvider ---------------------------------------------------------------------------------
    IFACEMETHODIMP Invoke() override { return Act(kActInvoke); }

    // --- IToggleProvider ---------------------------------------------------------------------------------
    IFACEMETHODIMP Toggle() override { return Act(kActInvoke); }
    IFACEMETHODIMP get_ToggleState(ToggleState* out) override {
        if (!out) return E_POINTER;
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        *out = e.checked ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // --- ISelectionItemProvider --------------------------------------------------------------------------
    IFACEMETHODIMP Select() override { return Act(kActSelect); }
    IFACEMETHODIMP AddToSelection() override {
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        if (e.selected) return S_OK;
        // Single selection: adding would make two, unless nothing is selected yet.
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            for (const auto& o : state_->elements)
                if (o.kind == Kind::Radio && o.radioSet == e.radioSet && o.selected) return UIA_E_INVALIDOPERATION;
        }
        return Act(kActSelect);
    }
    IFACEMETHODIMP RemoveFromSelection() override {
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        return e.selected ? UIA_E_INVALIDOPERATION : S_OK; // A selection is required.
    }
    IFACEMETHODIMP get_IsSelected(BOOL* out) override {
        if (!out) return E_POINTER;
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        *out = e.selected;
        return S_OK;
    }
    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        const Element* e = state_->Find(id_);
        if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
        if (e->radioSet != ui::kSetNone && state_->Find(SetNodeId(e->radioSet)))
            *out = static_cast<IRawElementProviderSimple*>(Get(state_, SetNodeId(e->radioSet)));
        return S_OK;
    }

    // --- ISelectionProvider (radio-set containers) ---------------------------------------------------------
    IFACEMETHODIMP GetSelection(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        const Element* set = state_->Find(id_);
        if (!set) return UIA_E_ELEMENTNOTAVAILABLE;
        std::vector<int> picked;
        for (const auto& o : state_->elements)
            if (o.kind == Kind::Radio && o.radioSet == set->radioSet && o.selected) picked.push_back(o.id);
        SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, ULONG(picked.size()));
        if (!sa) return E_OUTOFMEMORY;
        for (LONG i = 0; i < LONG(picked.size()); ++i) {
            Fragment* f = Get(state_, picked[size_t(i)]);
            SafeArrayPutElement(sa, &i, static_cast<IRawElementProviderSimple*>(f)); // AddRefs it.
            f->Release();
        }
        *out = sa;
        return S_OK;
    }
    IFACEMETHODIMP get_CanSelectMultiple(BOOL* out) override {
        if (!out) return E_POINTER;
        *out = FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_IsSelectionRequired(BOOL* out) override {
        if (!out) return E_POINTER;
        *out = TRUE;
        return S_OK;
    }

    // --- IExpandCollapseProvider -----------------------------------------------------------------------------
    IFACEMETHODIMP Expand() override { return Act(kActExpand); }
    IFACEMETHODIMP Collapse() override { return Act(kActCollapse); }
    IFACEMETHODIMP get_ExpandCollapseState(ExpandCollapseState* out) override {
        if (!out) return E_POINTER;
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        *out = !e.expandable ? ExpandCollapseState_LeafNode : e.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
        return S_OK;
    }

    // --- IRangeValueProvider (read-only, 0..100 %) -------------------------------------------------------------
    IFACEMETHODIMP SetValue(double) override { return UIA_E_INVALIDOPERATION; }
    IFACEMETHODIMP get_Value(double* out) override {
        if (!out) return E_POINTER;
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        *out = e.value < 0 ? 0 : double(e.value) * 100.0;
        return S_OK;
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL* out) override { return Const(out, TRUE); }
    IFACEMETHODIMP get_Maximum(double* out) override { return Const(out, 100.0); }
    IFACEMETHODIMP get_Minimum(double* out) override { return Const(out, 0.0); }
    IFACEMETHODIMP get_LargeChange(double* out) override { return Const(out, 0.0); }
    IFACEMETHODIMP get_SmallChange(double* out) override { return Const(out, 0.0); }

private:
    ~Fragment() = default;

    template <typename T>
    static HRESULT Const(T* out, T value) {
        if (!out) return E_POINTER;
        *out = value;
        return S_OK;
    }

    static void SetString(VARIANT* v, const wchar_t* s) {
        v->vt = VT_BSTR;
        v->bstrVal = SysAllocString(s);
    }
    static void SetBool(VARIANT* v, bool b) {
        v->vt = VT_BOOL;
        v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    }

    // First / last child of `parent`, or INT_MIN. Caller holds the mutex.
    int Child(int parent, bool last) const {
        const auto& list = state_->elements;
        int found = INT_MIN;
        for (const auto& e : list) {
            if (e.parent != parent) continue;
            found = e.id;
            if (!last) break;
        }
        return found;
    }

    HRESULT Snap(Element* out) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        const Element* e = state_->Find(id_);
        if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
        *out = *e;
        return S_OK;
    }

    HRESULT Post(Action action) const {
        HWND hwnd;
        UINT msg;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
            hwnd = state_->hwnd;
            msg = state_->actionMsg;
        }
        return PostMessageW(hwnd, msg, action, LPARAM(id_)) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    // Checks the snapshot, then posts the action to the UI thread (which checks again).
    HRESULT Act(Action action) const {
        Element e;
        HRESULT hr = Snap(&e);
        if (FAILED(hr)) return hr;
        if (!e.enabled) return UIA_E_ELEMENTNOTENABLED;
        if (action == kActSelect && e.selected) return S_OK;
        if (action == kActExpand && e.expanded) return S_OK;
        if (action == kActCollapse && !e.expanded) return S_OK;
        return Post(action);
    }

    std::atomic<ULONG> refs_{1};
    std::shared_ptr<State> state_;
    const int id_;
};

// The visible elements, with a container per radio set and parents that exist.
std::vector<Element> BuildTree(const std::vector<Element>& all) {
    std::vector<Element> out;
    out.reserve(all.size() + 4);
    std::vector<int> setsDone;
    for (const auto& e : all) {
        if (!e.visible || e.id == ui::kNone) continue;
        if (e.kind == Kind::Radio && e.radioSet != ui::kSetNone &&
            std::find(setsDone.begin(), setsDone.end(), e.radioSet) == setsDone.end()) {
            setsDone.push_back(e.radioSet);
            Element set;
            set.id = SetNodeId(e.radioSet);
            set.kind = Kind::Group;
            set.name = SetName(e.radioSet);
            set.parent = e.parent;
            set.radioSet = e.radioSet;
            set.rect = e.rect;
            set.clip = e.clip;
            set.enabled = false;
            for (const auto& o : all) {
                if (o.kind != Kind::Radio || o.radioSet != e.radioSet || !o.visible) continue;
                set.rect = Union(set.rect, o.rect);
                set.clip = Union(set.clip, o.clip);
                set.enabled = set.enabled || o.enabled;
            }
            out.push_back(set);
        }
        out.push_back(e);
        if (e.kind == Kind::Radio && e.radioSet != ui::kSetNone) out.back().parent = SetNodeId(e.radioSet);
    }
    for (auto& e : out) {
        bool found = false;
        for (const auto& p : out) found = found || (p.id == e.parent && p.id != e.id);
        if (!found) e.parent = kRoot;
    }
    return out;
}

void RaiseProperty(IRawElementProviderSimple* p, PROPERTYID prop, VARIANT oldValue, VARIANT newValue) {
    UiaRaiseAutomationPropertyChangedEvent(p, prop, oldValue, newValue);
}

VARIANT Bool(bool b) {
    VARIANT v;
    v.vt = VT_BOOL;
    v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return v;
}
VARIANT Int(long i) {
    VARIANT v;
    v.vt = VT_I4;
    v.lVal = i;
    return v;
}
VARIANT Dbl(double d) {
    VARIANT v;
    v.vt = VT_R8;
    v.dblVal = d;
    return v;
}

} // namespace

UiaHost::UiaHost(HWND hwnd, UiaSource source) : state_(std::make_shared<State>()), source_(std::move(source)) {
    state_->hwnd = hwnd;
    state_->actionMsg = RegisterWindowMessageW(L"MyCamUiaAction");
}

UiaHost::~UiaHost() { Disconnect(); }

bool UiaHost::HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (msg == WM_GETOBJECT) {
        if (static_cast<long>(lp) != static_cast<long>(UiaRootObjectId)) return false;
        bool first;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->alive) return false;
            first = !state_->requested;
            state_->requested = true;
        }
        if (first) Update();
        Fragment* root;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            root = Fragment::Get(state_, kRoot);
        }
        *result = UiaReturnRawElementProvider(state_->hwnd, wp, lp, static_cast<IRawElementProviderSimple*>(root));
        root->Release();
        return true;
    }
    if (msg != state_->actionMsg || state_->actionMsg == 0) return false;
    *result = 0;
    // Runs on the UI thread: check against the live element list, then act like a click / key press.
    const int id = int(lp);    const std::vector<Element> list = source_.elements ? source_.elements() : std::vector<Element>{};
    const Element* e = ui::FindElement(list, id);
    if (!e || !e->visible || !e->enabled) return true;
    switch (wp) {
    case kActFocus:
        if (source_.focus) source_.focus(id);
        break;
    case kActSelect:
        if (!e->selected && source_.invoke) source_.invoke(id);
        break;
    case kActExpand:
        if (!e->expanded && source_.invoke) source_.invoke(id);
        break;
    case kActCollapse:
        if (e->expanded && source_.invoke) source_.invoke(id);
        break;
    case kActInvoke:
        if (source_.invoke) source_.invoke(id); // May close the window: touch nothing afterwards.
        break;
    }
    return true;
}

void UiaHost::Update() {
    HWND hwnd;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive || !state_->requested) return;
        hwnd = state_->hwnd;
    }
    if (!IsWindow(hwnd)) return;

    // New snapshot, built outside the lock.
    std::vector<Element> next = BuildTree(source_.elements ? source_.elements() : std::vector<Element>{});
    std::vector<RECT> screen;
    screen.reserve(next.size());
    for (const auto& e : next) {
        const ui::Box seen = Intersect(e.rect, e.clip);
        const bool clipped = seen.r > seen.l && seen.b > seen.t;
        screen.push_back(source_.toScreen ? source_.toScreen(clipped ? seen : e.rect) : RECT{});
    }
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    const int focused = source_.focusedId ? source_.focusedId() : ui::kNone;
    const bool windowFocused = GetFocus() == hwnd;

    std::vector<Element> prev;
    int prevFocused;
    bool prevWindowFocused;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return;
        prev.swap(state_->elements);
        state_->elements = next;
        state_->screen = std::move(screen);
        state_->origin = origin;
        prevFocused = state_->focused;
        prevWindowFocused = state_->windowFocused;
        state_->focused = focused;
        state_->windowFocused = windowFocused;
    }

    if (!UiaClientsAreListening()) return;

    // Providers for raising events (never raise while holding the mutex: UIA may call back).
    auto provider = [&](int id) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return Fragment::Get(state_, id);
    };
    auto raise = [&](int id, auto&& fn) {
        Fragment* f = provider(id);
        fn(static_cast<IRawElementProviderSimple*>(f));
        f->Release();
    };

    // Structure: a parent whose children changed.
    std::map<int, std::vector<int>> before, after;
    for (const auto& e : prev) before[e.parent].push_back(e.id);
    for (const auto& e : next) after[e.parent].push_back(e.id);
    std::vector<int> invalid;
    for (const auto& [parent, kids] : after) {
        auto it = before.find(parent);
        if (it == before.end() ? !kids.empty() : it->second != kids) invalid.push_back(parent);
    }
    for (const auto& [parent, kids] : before) {
        if (after.find(parent) == after.end() && !kids.empty()) invalid.push_back(parent);
    }
    for (int parent : invalid) {
        if (parent != kRoot && !ui::FindElement(next, parent)) continue; // Gone itself: its parent covers it.
        raise(parent, [&](IRawElementProviderSimple* p) {
            if (parent == kRoot) {
                UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, nullptr, 0);
            } else {
                int rid[2] = {UiaAppendRuntimeId, parent};
                UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, rid, 2);
            }
        });
    }

    // State changes.
    for (const auto& e : next) {
        const Element* o = ui::FindElement(prev, e.id);
        if (!o) {
            if (e.liveRegion && !e.name.empty())
                raise(e.id, [](IRawElementProviderSimple* p) { UiaRaiseAutomationEvent(p, UIA_LiveRegionChangedEventId); });
            continue;
        }
        const bool valueMoved = e.kind == Kind::ProgressBar && e.value >= 0 && o->value >= 0 && int(o->value * 100) != int(e.value * 100);
        if (o->name == e.name && o->enabled == e.enabled && o->Offscreen() == e.Offscreen() && o->checked == e.checked &&
            o->selected == e.selected && o->expanded == e.expanded && !valueMoved)
            continue; // Nothing to tell (the common case on every paint).
        raise(e.id, [&](IRawElementProviderSimple* p) {
            if (o->name != e.name) {
                VARIANT a, b;
                a.vt = b.vt = VT_BSTR;
                a.bstrVal = SysAllocString(o->name.c_str());
                b.bstrVal = SysAllocString(e.name.c_str());
                RaiseProperty(p, UIA_NamePropertyId, a, b);
                SysFreeString(a.bstrVal);
                SysFreeString(b.bstrVal);
                if (e.liveRegion) UiaRaiseAutomationEvent(p, UIA_LiveRegionChangedEventId);
            }
            if (o->enabled != e.enabled) RaiseProperty(p, UIA_IsEnabledPropertyId, Bool(o->enabled), Bool(e.enabled));
            if (o->Offscreen() != e.Offscreen()) RaiseProperty(p, UIA_IsOffscreenPropertyId, Bool(o->Offscreen()), Bool(e.Offscreen()));
            if (e.kind == Kind::Checkbox && o->checked != e.checked)
                RaiseProperty(p, UIA_ToggleToggleStatePropertyId, Int(o->checked ? ToggleState_On : ToggleState_Off),
                              Int(e.checked ? ToggleState_On : ToggleState_Off));
            if (e.kind == Kind::Radio && o->selected != e.selected) {
                RaiseProperty(p, UIA_SelectionItemIsSelectedPropertyId, Bool(o->selected), Bool(e.selected));
                if (e.selected) UiaRaiseAutomationEvent(p, UIA_SelectionItem_ElementSelectedEventId);
            }
            if (e.kind == Kind::Group && e.expandable && o->expanded != e.expanded)
                RaiseProperty(p, UIA_ExpandCollapseExpandCollapseStatePropertyId,
                              Int(o->expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed),
                              Int(e.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed));
            if (valueMoved)
                RaiseProperty(p, UIA_RangeValueValuePropertyId, Dbl(o->value * 100.0), Dbl(e.value * 100.0));
        });
    }

    // Keyboard focus moved inside the window.
    if (windowFocused && focused != ui::kNone && ui::FindElement(next, focused) &&
        (focused != prevFocused || !prevWindowFocused)) {
        raise(focused, [](IRawElementProviderSimple* p) { UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId); });
    }
}

void UiaHost::Disconnect() {
    std::map<int, Fragment*> cache;
    HWND hwnd;
    bool requested;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) return;
        state_->alive = false;
        cache.swap(state_->cache);
        state_->elements.clear();
        state_->screen.clear();
        hwnd = state_->hwnd;
        requested = state_->requested;
    }
    if (requested) {
        if (IsWindow(hwnd)) UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
        for (auto& [id, f] : cache) UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(f));
    }
    for (auto& [id, f] : cache) f->Release(); // The cache's references; clients may still hold theirs.
}

std::wstring UiaAutomationId(int id) {
    using namespace ui;
    if (IsSetNode(id)) {
        switch (kSetNodeBase - id) {
        case kSetFacing: return L"SetCamera";
        case kSetQuality: return L"SetQuality";
        case kSetFps: return L"SetFrameRate";
        case kSetFocus: return L"SetFocus";
        }
        return L"Set" + std::to_wstring(kSetNodeBase - id);
    }
    switch (id) {
    case kCaptionMin: return L"Minimize";
    case kCaptionClose: return L"Close";
    case kGroupNow: return L"GroupNow";
    case kGroupCamera: return L"GroupCamera";
    case kGroupVideo: return L"GroupVideo";
    case kGroupPicture: return L"GroupPicture";
    case kGroupWifi: return L"GroupWifi";
    case kGroupTasks: return L"GroupTasks";
    case kStatusHeadline: return L"StatusHeadline";
    case kStatusDetail: return L"StatusDetail";
    case kPause: return L"Pause";
    case kLivePill: return L"LivePill";
    case kStatusProgress: return L"StatusProgress";
    case kShowPairing: return L"ShowPairing";
    case kScanNow: return L"ScanNow";
    case kBack: return L"CameraBack";
    case kFront: return L"CameraFront";
    case kCameraNote: return L"CameraNote";
    case kQ720: return L"Quality720p";
    case kQ1080: return L"Quality1080p";
    case kQ4K: return L"Quality4K";
    case kFps30: return L"Fps30";
    case kFps60: return L"Fps60";
    case kFps120: return L"Fps120";
    case kMirror: return L"Mirror";
    case kFill: return L"Fill";
    case kAutostart: return L"Autostart";
    case kWireless: return L"Wireless";
    case kForgetPhones: return L"ForgetPhones";
    case kScan: return L"Scan";
    case kScanProgress: return L"ScanProgress";
    case kScanResult: return L"ScanResult";
    case kScanList: return L"ScanList";
    case kReconnect: return L"Reconnect";
    case kOpenLog: return L"OpenLog";
    case kPreview: return L"Preview";
    case kStatusSentence: return L"StatusSentence";
    case kZoomOut: return L"ZoomOut";
    case kZoomValue: return L"ZoomValue";
    case kZoomIn: return L"ZoomIn";
    case kZoomReset: return L"ZoomReset";
    case kEvDown: return L"BrightnessDown";
    case kEvValue: return L"BrightnessValue";
    case kEvUp: return L"BrightnessUp";
    case kFocusAuto: return L"FocusAuto";
    case kFocusLock: return L"FocusLock";
    case kTorch: return L"Torch";
    case kAuto: return L"AutoReset";
    case kNowMode: return L"NowMode";
    case kControlsNote: return L"ControlsNote";
    case kPairTitle: return L"PairTitle";
    case kPairCode: return L"PairCode";
    case kPairHint: return L"PairHint";
    case kPairProgress: return L"PairProgress";
    case kPairCancel: return L"PairCancel";
    }
    int part = 0;
    const int row = ScanRowIndex(id, &part);
    if (row >= 0) return std::wstring(part == 1 ? L"ScanConnect" : L"ScanRow") + std::to_wstring(row);
    return L"Element" + std::to_wstring(id);
}

} // namespace mycam

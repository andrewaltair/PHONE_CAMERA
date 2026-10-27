#pragma once
// UI Automation provider for the custom-drawn companion windows (docs/redesign.md §8.4, W2). One UIA fragment
// per ui::Element (ui_model.h), hierarchical by Element::parent; each radio set gets a synthetic container
// with the Selection pattern. Used by the settings window and the pairing dialog.
//
// Threading: UIA calls arrive on its own threads. They only read a snapshot of the element list, which
// the UI thread refreshes (Update) under a mutex. Actions (Invoke, Toggle, Select, Expand, SetFocus) are
// posted to the window and run on the UI thread, so every state change happens there.
//
// Lifetime: providers look their element up by id on every call and return UIA_E_ELEMENTNOTAVAILABLE once
// it is gone. Disconnect() (on WM_DESTROY) detaches every provider handed out.

#include <windows.h>

#include <functional>
#include <memory>
#include <vector>

#include "ui_model.h"

namespace mycam {

// What the provider needs from a window. Every callback is called on the UI thread only.
struct UiaSource {
    std::function<std::vector<ui::Element>()> elements;
    std::function<bool(int)> invoke;          // Click / Space on the element.
    std::function<bool(int)> focus;           // Move keyboard focus.
    std::function<int()> focusedId;
    std::function<RECT(const ui::Box&)> toScreen; // Client DIPs -> screen pixels.
    // RangeValue::SetValue on a slider, in the control's own scale. Empty where there are no sliders.
    std::function<bool(int, double)> setValue;
};

class UiaHost {
public:
    UiaHost(HWND hwnd, UiaSource source);
    ~UiaHost();
    UiaHost(const UiaHost&) = delete;
    UiaHost& operator=(const UiaHost&) = delete;

    // Call first thing in the window procedure: handles WM_GETOBJECT and the posted UIA actions.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
    // UI thread, after every layout / paint / focus change: refreshes the snapshot and raises the events
    // (focus, toggle, selection, expand/collapse, live region, structure). Cheap no-op until a UIA client
    // has asked for the window's provider.
    void Update();
    // WM_DESTROY: disconnects every provider. Safe to call twice.
    void Disconnect();

    struct State;

private:
    std::shared_ptr<State> state_;
    UiaSource source_;
};

// Stable AutomationId for an element id ("Pause", "Mirror", "GroupVideo", "SetQuality", ...).
std::wstring UiaAutomationId(int id);

} // namespace mycam

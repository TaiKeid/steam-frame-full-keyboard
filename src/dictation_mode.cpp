#include "framekeyboard/dictation_mode.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace framekeyboard {
namespace {
bool removed(const Key& key) {
    static const std::set<std::string> actions{
        "PrintScreen",  "ScrollLock", "Pause",     "Tab",     "Insert",   "ControlLeft",
        "ControlRight", "MetaLeft",   "MetaRight", "AltLeft", "AltRight", "ContextMenu"};
    if (key.action_kind == ActionKind::Shortcut || key.action_kind == ActionKind::Emoji) {
        return true;
    }
    if (key.action_kind != ActionKind::Key) {
        return false;
    }
    const bool function = key.action.size() >= 2 && key.action[0] == 'F' &&
                          std::all_of(key.action.begin() + 1, key.action.end(),
                                      [](char c) { return c >= '0' && c <= '9'; });
    return function || actions.contains(key.action);
}
std::optional<Rect> bounds_of(const Layout& main, std::initializer_list<const char*> actions) {
    std::optional<Rect> result;
    for (const auto& key : main.keys) {
        if (key.action_kind != ActionKind::Key ||
            std::none_of(actions.begin(), actions.end(),
                         [&](const char* a) { return key.action == a; })) {
            continue;
        }
        if (!result) {
            result = key.bounds;
            continue;
        }
        const double left = std::min(result->x, key.bounds.x);
        const double top = std::min(result->y, key.bounds.y);
        const double right = std::max(result->x + result->width, key.bounds.x + key.bounds.width);
        const double bottom = std::max(result->y + result->height, key.bounds.y + key.bounds.height);
        result = Rect{left, top, right - left, bottom - top};
    }
    return result;
}
Key dictation_key(const std::string& action, const std::string& label, Rect bounds, Icon icon) {
    Key key;
    key.id = "Dictation-" + action;
    key.label = label;
    key.bounds = bounds;
    key.action_kind = ActionKind::Dictation;
    key.action = action;
    key.icon = icon;
    return key;
}
} // namespace

DictationLayout dictation_layout(const Layout& main, double visible_width) {
    DictationLayout result;
    result.layout.id = main.id + "-dictation";
    result.layout.name = main.name;
    result.layout.language = main.language;
    result.layout.width = main.width;
    result.layout.height = main.height;
    std::optional<Rect> paste;
    for (const auto& key : main.keys) {
        if (key.action_kind == ActionKind::Shortcut && key.action == "paste") {
            paste = key.bounds;
        }
        if (!removed(key)) {
            auto copy = key;
            if (copy.action_kind == ActionKind::Key && copy.action == "Numpad0") {
                copy.secondary_label.clear(); // Insert does nothing in the editor; only 0 remains
            }
            result.layout.keys.push_back(std::move(copy));
        }
    }
    // Send spans the right-hand modifier group. Layouts without one, such as
    // flick, give it Tab's freed slot.
    auto send = bounds_of(main, {"AltRight", "MetaRight", "ContextMenu", "ControlRight"});
    if (!send) {
        send = bounds_of(main, {"Tab"});
    }
    if (send) {
        result.layout.keys.push_back(dictation_key("send", "Send", *send, Icon::None));
    }
    if (paste) {
        result.layout.keys.push_back(dictation_key("undo", "Undo", *paste, Icon::Undo));
    }
    // The editor starts right of Esc, or right of the side column without one.
    double left = 0;
    for (const auto& key : main.keys) {
        if ((key.action_kind == ActionKind::Key && key.action == "Escape") ||
            (key.action_kind != ActionKind::Key && key.action_kind != ActionKind::Flick &&
             key.bounds.y < 44)) {
            left = std::max(left, key.bounds.x + key.bounds.width);
        }
    }
    const auto row = bounds_of(main, {"F1", "F12", "Escape"});
    const double bottom = row ? row->y + row->height : 44;
    const double right = std::clamp(visible_width, left + 120, main.width);
    result.editor = {left + 12, -dictation_editor_rise, right - left - 12,
                     bottom + dictation_editor_rise + dictation_editor_drop};
    return result;
}
} // namespace framekeyboard

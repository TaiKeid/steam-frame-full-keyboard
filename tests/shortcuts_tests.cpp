#include "framekeyboard/app.hpp"
#include "framekeyboard/app_names.hpp"
#include "framekeyboard/shortcut_editor.hpp"
#include "framekeyboard/shortcuts.hpp"
#include "framekeyboard/stream_app.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-shortcuts-tests-XXXXXX";
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
// The design's example file.
const char* const example = R"({
  "schema_version": 1,
  "profiles": [
    {
      "id": "b1",
      "name": "Editor",
      "buttons": [
        {
          "label": "Save\nall",
          "action": {
            "kind": "keys",
            "chords": [
              {"modifiers": ["ControlLeft"], "key": "KeyK"},
              {"key": "KeyS"}
            ]
          }
        },
        {
          "label": "Build",
          "action": {
            "kind": "macro",
            "steps": [
              {"keys": [{"modifiers": ["ControlLeft", "ShiftLeft"], "key": "KeyB"}]},
              {"wait_ms": 500},
              {"keys": [{"key": "KeyL"}, {"key": "KeyS"}, {"key": "Enter"}]}
            ],
            "repeat": {"mode": "times", "count": 3}
          }
        },
        {"label": "Term", "action": {"kind": "run", "script": "open-terminal.sh"}}
      ]
    }
  ],
  "apps": [
    {"kind": "desktop", "id": "org.kde.kate", "name": "Kate", "profile": "b1"}
  ],
  "free_profile": "b1"
})";
ShortcutButton keys_button(std::string label, std::vector<ShortcutChord> chords) {
    ShortcutButton button;
    button.label = std::move(label);
    button.action.kind = ShortcutAction::Kind::Keys;
    button.action.chords = std::move(chords);
    return button;
}
// A small valid model to change one thing at a time.
ShortcutModel sample() {
    ShortcutModel model;
    model.profiles.push_back({"b1", "One", {keys_button("Copy", {{{"ControlLeft"}, "KeyC"}})}});
    model.profiles.push_back({"b2", "Two", {}});
    return model;
}
// The model is refused by validation and by loading its file, and the
// reason never quotes `secret`, the user's own content.
void refused(const ShortcutModel& model, const std::string& what, const std::string& secret = "") {
    const auto reason = validate_shortcuts(model);
    require(reason.has_value(), what + ": validation refuses it");
    require(secret.empty() || reason->find(secret) == std::string::npos,
            what + ": the reason quotes no content");
    try {
        parse_shortcuts(serialize_shortcuts(model));
    } catch (const std::exception& error) {
        require(secret.empty() || std::string(error.what()).find(secret) == std::string::npos,
                what + ": the load reason quotes no content");
        return;
    }
    throw std::runtime_error(what + ": loading refuses it");
}
void accepted(const ShortcutModel& model, const std::string& what) {
    const auto reason = validate_shortcuts(model);
    require(!reason, what + ": accepted (" + reason.value_or("") + ")");
    require(parse_shortcuts(serialize_shortcuts(model)) == model, what + ": survives a round trip");
}
void rejects_json(const std::string& json, const std::string& what, const std::string& secret = "") {
    try {
        parse_shortcuts(json);
    } catch (const std::runtime_error& error) {
        if (!secret.empty() && std::string(error.what()).find(secret) != std::string::npos) {
            throw std::runtime_error(what + ": the reason quotes the file");
        }
        return;
    }
    throw std::runtime_error(what + ": refused");
}
std::string repeated(const std::string& piece, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        result += piece;
    }
    return result;
}
ShortcutButton macro_button(std::vector<MacroStep> steps, MacroRepeat repeat = {}) {
    ShortcutButton button;
    button.label = "M";
    button.action.kind = ShortcutAction::Kind::Macro;
    button.action.steps = std::move(steps);
    button.action.repeat = repeat;
    return button;
}
MacroStep keys_step(std::vector<ShortcutChord> keys) {
    MacroStep step;
    step.kind = MacroStep::Kind::Keys;
    step.keys = std::move(keys);
    return step;
}
MacroStep wait_step(int ms) {
    MacroStep step;
    step.kind = MacroStep::Kind::Wait;
    step.wait_ms = ms;
    return step;
}
ShortcutButton run_button(std::string script, std::string command) {
    ShortcutButton button;
    button.label = "R";
    button.action.kind = ShortcutAction::Kind::Run;
    button.action.script = std::move(script);
    button.action.command = std::move(command);
    return button;
}

void example_file() {
    const auto model = parse_shortcuts(example);
    require(model.profiles.size() == 1 && model.profiles[0].id == "b1" &&
                model.profiles[0].name == "Editor",
            "the design's bar");
    const auto& buttons = model.profiles[0].buttons;
    require(buttons.size() == 3 && buttons[0].label == "Save\nall", "its buttons and a two-line label");
    const std::vector<ShortcutChord> save_all{{{"ControlLeft"}, "KeyK"}, {{}, "KeyS"}};
    require(buttons[0].action.kind == ShortcutAction::Kind::Keys && buttons[0].action.chords == save_all,
            "a shortcut's combinations in order");
    const auto& macro = buttons[1].action;
    const std::vector<ShortcutChord> typed{{{}, "KeyL"}, {{}, "KeyS"}, {{}, "Enter"}};
    require(macro.kind == ShortcutAction::Kind::Macro && macro.steps.size() == 3 &&
                macro.steps[0].kind == MacroStep::Kind::Keys && macro.steps[0].keys.size() == 1 &&
                macro.steps[0].keys[0].modifiers.size() == 2 &&
                macro.steps[1].kind == MacroStep::Kind::Wait && macro.steps[1].wait_ms == 500 &&
                macro.steps[2].keys == typed && macro.repeat == MacroRepeat{MacroRepeat::Mode::Times, 3},
            "a macro's steps, a Keys step's combinations in order, and its repeat");
    require(buttons[2].action.kind == ShortcutAction::Kind::Run &&
                buttons[2].action.script == "open-terminal.sh" && buttons[2].action.command.empty(),
            "a script button");
    require(model.apps.size() == 1 &&
                model.apps[0] == AppTie{{AppIdentity::Kind::Desktop, "org.kde.kate"}, "Kate", "b1"},
            "an assigned app");
    require(model.free_profile == "b1", "the free bar");
    require(parse_shortcuts(serialize_shortcuts(model)) == model, "the example survives a round trip");
    // Unknown members are ignored at every level.
    const auto extra = parse_shortcuts(
        R"({"schema_version":1,"later":true,"profiles":[{"id":"a","name":"A","color":"red","buttons":[)"
        R"({"label":"x","icon":"y","action":{"kind":"keys","hold":1,"chords":[{"key":"F13","x":1}]}}]}]})");
    require(extra.profiles[0].buttons[0].action.chords[0].key == "F13", "unknown members are ignored");
    require(parse_shortcuts(R"({"schema_version":1,"profiles":[]})").apps.empty(),
            "apps and free_profile are optional");
    // A macro without repeat runs once.
    const auto once =
        parse_shortcuts(R"({"schema_version":1,"profiles":[{"id":"a","name":"","buttons":[{"label":"",)"
                        R"("action":{"kind":"macro","steps":[{"wait_ms":100}]}}]}]})");
    require(once.profiles[0].buttons[0].action.repeat.mode == MacroRepeat::Mode::Once,
            "a macro without repeat runs once");
    // Modifiers alone, and every modifier and the new F keys by name.
    ShortcutModel keys = sample();
    keys.profiles[0].buttons.push_back(keys_button("Super", {{{"MetaLeft"}, ""}}));
    keys.profiles[0].buttons.push_back(
        keys_button("All", {{{"ControlLeft", "ControlRight", "ShiftLeft", "ShiftRight", "AltLeft",
                              "AltRight", "MetaLeft", "MetaRight"},
                             "F24"}}));
    accepted(keys, "modifiers alone and all eight modifiers");
}

// A button the user has not finished: its action is empty, valid, and does nothing.
void empty_actions() {
    const auto model =
        parse_shortcuts(R"({"schema_version":1,"profiles":[{"id":"a","name":"A","buttons":[)"
                        R"({"label":"k","action":{"kind":"keys","chords":[]}},)"
                        R"({"label":"m","action":{"kind":"macro","steps":[]}},)"
                        R"({"label":"r","action":{"kind":"run"}}]}]})");
    const auto& buttons = model.profiles[0].buttons;
    require(buttons.size() == 3 && std::all_of(buttons.begin(), buttons.end(),
                                               [](const ShortcutButton& button) {
                                                   return shortcut_action_empty(button.action);
                                               }),
            "a shortcut without combinations, a macro without steps and Run with neither are empty");
    require(parse_shortcuts(serialize_shortcuts(model)) == model, "empty actions survive a round trip");
    auto finished = sample();
    require(!shortcut_action_empty(finished.profiles[0].buttons[0].action),
            "a shortcut with keys is not");
    finished.profiles[0].buttons.push_back(run_button("", "true"));
    require(!shortcut_action_empty(finished.profiles[0].buttons[1].action), "nor a command");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","name":"A","buttons":[)"
                 R"({"label":"r","action":{"kind":"run","script":""}}]}]})",
                 "an empty script name");
}
void invalid_files() {
    rejects_json("{broken", "not JSON");
    rejects_json("[]", "not an object");
    rejects_json("\"text\"", "a string");
    rejects_json(R"({"profiles":[]})", "no schema_version");
    rejects_json(R"({"schema_version":2,"profiles":[]})", "an unknown schema_version");
    rejects_json(R"({"schema_version":"1","profiles":[]})", "a schema_version that is a string");
    rejects_json(R"({"schema_version":1.0,"profiles":[]})", "a schema_version that is not an integer");
    rejects_json(R"({"schema_version":1})", "no bars");
    rejects_json(R"({"schema_version":1,"profiles":{}})", "bars not an array");
    rejects_json(R"({"schema_version":1,"profiles":[1]})", "a bar that is not an object");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","buttons":[]}]})", "a bar without a name");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","name":"A"}]})", "a bar without buttons");
    const std::string head = R"({"schema_version":1,"profiles":[{"id":"a","name":"A","buttons":[)";
    const std::string tail = "]}]}";
    auto button = [&](const std::string& action) {
        return head + R"({"label":"x","action":)" + action + "}" + tail;
    };
    rejects_json(head + R"({"action":{"kind":"keys","chords":[{"key":"KeyA"}]}})" + tail,
                 "a button without a label");
    rejects_json(head + R"({"label":"x"})" + tail, "a button without an action");
    rejects_json(button(R"({"kind":"press","chords":[]})"), "an unknown action kind");
    rejects_json(button(R"({"chords":[{"key":"KeyA"}]})"), "an action without a kind");
    rejects_json(button(R"({"kind":"keys"})"), "a shortcut without chords");
    rejects_json(button(R"({"kind":"keys","chords":[{}]})"), "an empty chord");
    rejects_json(button(R"({"kind":"keys","chords":[{"key":""}]})"), "an empty key");
    rejects_json(button(R"({"kind":"keys","chords":[{"key":"KeyAA"}]})"), "an unknown key name");
    rejects_json(button(R"({"kind":"keys","chords":[{"key":29}]})"), "a raw key code");
    rejects_json(button(R"({"kind":"keys","chords":[{"modifiers":["Ctrl"],"key":"KeyA"}]})"),
                 "an unknown modifier name");
    rejects_json(button(R"({"kind":"keys","chords":[{"modifiers":["KeyB"],"key":"KeyA"}]})"),
                 "a key as a modifier");
    rejects_json(button(R"({"kind":"keys","chords":[{"modifiers":["ShiftLeft"],"key":"ControlLeft"}]})"),
                 "a modifier as the key");
    rejects_json(
        button(R"({"kind":"keys","chords":[{"modifiers":["ShiftLeft","ShiftLeft"],"key":"KeyA"}]})"),
        "a repeated modifier");
    // R330: the locks are the keyboard's own switches, never sent.
    rejects_json(button(R"({"kind":"keys","chords":[{"key":"NumLock"}]})"), "Num Lock as a key");
    rejects_json(button(R"({"kind":"keys","chords":[{"modifiers":["ShiftLeft"],"key":"CapsLock"}]})"),
                 "Caps Lock as a key");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":[{"key":"KeyA"},{"key":"CapsLock"}]}]})"),
                 "Caps Lock in a macro's keys");
    rejects_json(button(R"({"kind":"macro"})"), "a macro without steps");
    rejects_json(button(R"({"kind":"macro","steps":[{}]})"), "a step of no kind");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":[{"key":"KeyA"}],"wait_ms":100}]})"),
                 "a step of two kinds");
    rejects_json(button(R"({"kind":"macro","steps":[{"wait_ms":"100"}]})"), "a wait that is a string");
    rejects_json(button(R"({"kind":"macro","steps":[{"wait_ms":100.5}]})"), "a fractional wait");
    // Macros press keys only: a text step, even beside a valid one, refuses the file.
    rejects_json(button(R"({"kind":"macro","steps":[{"text":"SecretText"}]})"), "a text step",
                 "SecretText");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":[{"key":"KeyA"}]},{"text":""}]})"),
                 "a text step after a keys step");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":{"key":"KeyA"}}]})"),
                 "a keys step that is one combination, not a list");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":[]}]})"), "a keys step with nothing");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":["KeyA"]}]})"),
                 "a keys step of names, not combinations");
    rejects_json(button(R"({"kind":"macro","steps":[{"keys":[{"key":"Nope"}]}]})"),
                 "an unknown key in a step");
    rejects_json(button(R"({"kind":"macro","steps":[{"wait_ms":100}],"repeat":{"mode":"forever"}})"),
                 "an unknown repeat mode");
    rejects_json(button(R"({"kind":"macro","steps":[{"wait_ms":100}],"repeat":{"mode":"times"}})"),
                 "times without a count");
    rejects_json(button(R"({"kind":"run","script":"a.sh","command":"ls"})"),
                 "a run with both script and command");
    rejects_json(button(R"({"kind":"run","script":"bin/a.sh"})"), "a script with a folder part");
    rejects_json(button(R"({"kind":"run","script":".."})"), "a script that is the parent folder");
    rejects_json(button(R"({"kind":"run","command":"ls\nrm"})"), "a command of two lines");
    rejects_json(button(R"({"kind":"run","command":""})"), "an empty command");
    const std::string bar = R"({"schema_version":1,"profiles":[{"id":"a","name":"A","buttons":[]}],)";
    rejects_json(bar + R"("apps":[{"kind":"flatpak","id":"x","profile":"a"}]})", "an unknown app kind");
    rejects_json(bar + R"("apps":[{"kind":"program","id":"x","profile":"b"}]})", "a tie to no bar");
    rejects_json(bar + R"("apps":[{"kind":"program","id":"","profile":"a"}]})", "an empty app ID");
    rejects_json(bar + R"("apps":[{"kind":"program","id":"/usr/bin/x","profile":"a"}]})",
                 "a program with a folder part");
    rejects_json(bar + R"("apps":[{"kind":"steam","id":"abc","profile":"a"}]})",
                 "a Steam ID not a number");
    rejects_json(bar + R"("apps":[{"kind":"steam","id":"4294967296","profile":"a"}]})",
                 "a Steam ID past 32 bits");
    rejects_json(bar + R"("apps":[{"kind":"program","id":"x","profile":"a"},)"
                       R"({"kind":"program","id":"x","profile":"a"}]})",
                 "an app assigned twice");
    rejects_json(bar + R"("free_profile":"b"})", "a free bar that does not exist");
    rejects_json(bar + R"("free_profile":""})", "an empty free bar");
    rejects_json(bar + R"("free_profile":1})", "a free bar that is a number");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","name":"A","buttons":[]},)"
                 R"({"id":"a","name":"B","buttons":[]}]})",
                 "two bars with one ID");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a b","name":"A","buttons":[]}]})",
                 "a bar ID with a space");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"","name":"A","buttons":[]}]})",
                 "an empty bar ID");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","name":"A\u0007","buttons":[]}]})",
                 "a control character in a name");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","name":"A\u0000B","buttons":[]}]})",
                 "a NUL in a name");
    rejects_json(R"({"schema_version":1,"profiles":[{"id":"a","name":"A\u0085","buttons":[]}]})",
                 "a C1 control character in a name");
    rejects_json(std::string(R"({"schema_version":1,"profiles":[{"id":"a","name":"A)") + "\xff" +
                     R"(","buttons":[]}]})",
                 "invalid UTF-8");
    rejects_json(head + R"({"label":"a\nb\nc","action":{"kind":"keys","chords":[{"key":"KeyA"}]}})" +
                     tail,
                 "a label of three lines");
    // The editor's own models use the same rules.
    ShortcutModel model = sample();
    model.profiles[0].buttons[0].label = std::string("bad") + '\xc3';
    refused(model, "an invalid UTF-8 label in a model");
    model = sample();
    model.profiles[0].buttons[0].action.chords[0].key = "SecretKeyName";
    refused(model, "an unknown key name", "SecretKeyName");
}

void limits() {
    ShortcutModel model = sample();
    // Bars: 32.
    model.profiles.clear();
    for (int i = 0; i < 32; ++i) {
        model.profiles.push_back({"b" + std::to_string(i), "Bar", {}});
    }
    accepted(model, "32 profiles");
    model.profiles.push_back({"b32", "Bar", {}});
    refused(model, "33 profiles");
    // Buttons in a bar: 64.
    model = sample();
    model.profiles[1].buttons.assign(64, keys_button("K", {{{}, "KeyA"}}));
    accepted(model, "64 buttons");
    model.profiles[1].buttons.push_back(keys_button("K", {{{}, "KeyA"}}));
    refused(model, "65 buttons");
    // Label: two lines, 24 characters in all, counted as characters.
    model = sample();
    model.profiles[0].buttons[0].label = repeated("é", 12) + "\n" + repeated("ж", 12);
    accepted(model, "a two-line label of 24 characters");
    model.profiles[0].buttons[0].label = repeated("Secret", 4) + "!";
    refused(model, "a label of 25 characters", "Secret");
    model.profiles[0].buttons[0].label = repeated("x", 24);
    accepted(model, "a one-line label of 24 characters");
    // Bar name: 32 characters.
    model = sample();
    model.profiles[0].name = repeated("名", 32);
    accepted(model, "a bar name of 32 characters");
    model.profiles[0].name = repeated("名", 32) + "!";
    refused(model, "a bar name of 33 characters");
    // Combinations in a shortcut: up to 4.
    model = sample();
    model.profiles[0].buttons[0].action.chords.assign(4, {{"ControlLeft"}, "KeyK"});
    accepted(model, "4 combinations");
    model.profiles[0].buttons[0].action.chords.push_back({{}, "KeyS"});
    refused(model, "5 combinations");
    // Steps in a macro: up to 32.
    model = sample();
    model.profiles[0].buttons[0] = macro_button(std::vector<MacroStep>(32, wait_step(100)));
    accepted(model, "32 steps");
    model.profiles[0].buttons[0].action.steps.push_back(wait_step(100));
    refused(model, "33 steps");
    // Combinations in a Keys step: 1 to 32.
    model.profiles[0].buttons[0] =
        macro_button({keys_step(std::vector<ShortcutChord>(32, {{"ShiftLeft"}, "KeyA"}))});
    accepted(model, "a keys step of 32 combinations");
    model.profiles[0].buttons[0].action.steps[0].keys.push_back({{}, "Enter"});
    refused(model, "a keys step of 33 combinations");
    model.profiles[0].buttons[0] = macro_button({keys_step({})});
    refused(model, "a keys step of no combinations");
    model.profiles[0].buttons[0] = macro_button({keys_step({{{}, "Nope"}})});
    refused(model, "a keys step with an unknown key");
    // Wait: 100 to 10,000 ms.
    model.profiles[0].buttons[0] = macro_button({wait_step(100), wait_step(10000)});
    accepted(model, "waits of 100 and 10000 ms");
    model.profiles[0].buttons[0] = macro_button({wait_step(99)});
    refused(model, "a wait of 99 ms");
    model.profiles[0].buttons[0] = macro_button({wait_step(10001)});
    refused(model, "a wait of 10001 ms");
    // Repeat count: 2 to 99.
    for (int count : {2, 99}) {
        model.profiles[0].buttons[0] = macro_button({wait_step(100)}, {MacroRepeat::Mode::Times, count});
        accepted(model, "a repeat count of " + std::to_string(count));
    }
    for (int count : {1, 100}) {
        model.profiles[0].buttons[0] = macro_button({wait_step(100)}, {MacroRepeat::Mode::Times, count});
        refused(model, "a repeat count of " + std::to_string(count));
    }
    model.profiles[0].buttons[0] = macro_button({wait_step(100)}, {MacroRepeat::Mode::WhileHeld, 0});
    accepted(model, "a macro repeated while held");
    // Command: 512 bytes, one line.
    model.profiles[0].buttons[0] = run_button("", "echo " + repeated("x", 507));
    accepted(model, "a command of 512 bytes");
    model.profiles[0].buttons[0] = run_button("", "SecretCommand " + repeated("x", 499));
    refused(model, "a command of 513 bytes", "SecretCommand");
    // Script name: 64 bytes, no folder part.
    model.profiles[0].buttons[0] = run_button(repeated("s", 61) + ".sh", "");
    accepted(model, "a script name of 64 bytes");
    model.profiles[0].buttons[0] = run_button("SecretScript" + repeated("s", 50) + ".sh", "");
    refused(model, "a script name of 65 bytes", "SecretScript");
    // App ties: 128.
    model = sample();
    for (int i = 0; i < 128; ++i) {
        model.apps.push_back({{AppIdentity::Kind::Steam, std::to_string(1000 + i)}, "", "b1"});
    }
    accepted(model, "128 assigned apps");
    model.apps.push_back({{AppIdentity::Kind::Steam, "5000"}, "", "b1"});
    refused(model, "129 assigned apps");
    // File: 512 KB.
    TemporaryDirectory directory;
    const auto path = shortcuts_path(directory.path);
    require(path == directory.path / "shortcuts.json", "the file is beside config.json");
    std::string padded = R"({"schema_version":1,"profiles":[]})";
    padded.insert(padded.size() - 1, std::string(shortcut_limits::file_bytes - padded.size(), ' '));
    require(padded.size() == shortcut_limits::file_bytes, "a file of exactly 512 KB");
    write(path, padded);
    ShortcutStore store(path);
    require(store.load().state == ShortcutsLoad::State::Valid, "a file of 512 KB loads");
    padded.insert(padded.size() - 1, " ");
    write(path, padded);
    const auto large = store.load();
    require(large.state == ShortcutsLoad::State::Invalid && large.model.profiles.empty(),
            "a larger file loads nothing");
}

void store() {
    TemporaryDirectory directory;
    const auto path = shortcuts_path(directory.path);
    ShortcutStore store(path);
    auto loaded = store.load();
    require(loaded.state == ShortcutsLoad::State::Missing && loaded.model == ShortcutModel{} &&
                loaded.reason.empty(),
            "no file is an empty model, and fine");
    // Save and reload.
    const auto model = parse_shortcuts(example);
    require(!store.save(model), "a valid model saves");
    struct stat info{};
    require(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "the file is readable by the user alone");
    loaded = ShortcutStore(path).load();
    require(loaded.state == ShortcutsLoad::State::Valid && loaded.model == model,
            "a save and reload give the same model");
    require(!fs::exists(path.string() + ".tmp") && !fs::exists(path.string() + ".bad"),
            "nothing is left beside it");
    // An invalid model is never written.
    auto broken = model;
    broken.free_profile = "gone";
    require(store.save(broken).has_value() && read(path) == serialize_shortcuts(model),
            "an invalid model is refused and the file is unchanged");
    // A missing folder is made.
    ShortcutStore nested(shortcuts_path(directory.path / "config"));
    require(!nested.save(model) && fs::exists(directory.path / "config/shortcuts.json"),
            "a missing config folder is made");
}

void bad_file_rule() {
    TemporaryDirectory directory;
    const auto path = shortcuts_path(directory.path);
    const auto bad = path.string() + ".bad";
    const std::string original = R"({"schema_version":1,"profiles":[{"id":"a","name":"MyPrivateName",)"
                                 R"("buttons":[{"label":"MyLabel","action":{"kind":"run",)"
                                 R"("command":"MyCommand","script":"x"}}]}]})";
    write(path, original);
    ShortcutStore store(path);
    const auto loaded = store.load();
    require(loaded.state == ShortcutsLoad::State::Invalid && loaded.model == ShortcutModel{} &&
                !loaded.reason.empty(),
            "an invalid file loads nothing, with a reason");
    for (const auto* secret : {"MyPrivateName", "MyLabel", "MyCommand"}) {
        require(loaded.reason.find(secret) == std::string::npos, "the reason quotes no content");
    }
    require(read(path) == original, "loading leaves an invalid file alone");
    // A refused save doesn't move it either.
    ShortcutModel invalid_model;
    invalid_model.free_profile = "none";
    require(store.save(invalid_model).has_value() && read(path) == original && !fs::exists(bad),
            "an invalid model moves nothing aside");
    // The first save moves it to shortcuts.json.bad, then writes.
    ShortcutModel model = sample();
    require(!store.save(model), "the first save after an invalid file");
    require(read(bad) == original, "the invalid file is kept as shortcuts.json.bad");
    require(ShortcutStore(path).load().model == model, "the new file is the saved model");
    // Later saves leave the .bad file alone.
    write(bad, "kept");
    model.profiles[0].name = "Changed";
    require(!store.save(model) && read(bad) == "kept", "the second save leaves .bad alone");
    // An unknown schema_version counts as invalid too.
    write(path, R"({"schema_version":2,"profiles":[]})");
    const auto future = store.load();
    require(future.state == ShortcutsLoad::State::Invalid, "an unknown schema_version is invalid");
    require(!store.save(model) && read(bad) == R"({"schema_version":2,"profiles":[]})",
            "and is moved aside by the next save");
    // A valid file is simply replaced.
    require(store.load().state == ShortcutsLoad::State::Valid, "the saved file loads");
    write(bad, "older");
    require(!store.save(sample()) && read(bad) == "older" && !fs::exists(path.string() + ".old"),
            "a file as this store left it is replaced, not moved");
}
// A hand edit made while the keyboard runs is kept, not overwritten.
void hand_edits() {
    TemporaryDirectory directory;
    const auto path = shortcuts_path(directory.path);
    const auto bad = path.string() + ".bad", old = path.string() + ".old";
    write(path, serialize_shortcuts(sample()));
    ShortcutStore store(path);
    require(store.load().state == ShortcutsLoad::State::Valid, "a valid file loads");
    // Edited with a typo after it was loaded.
    write(path, "{\"schema_version\":1,\"profiles\":[");
    auto model = sample();
    model.profiles[0].name = "Saved";
    require(!store.save(model) && read(bad) == "{\"schema_version\":1,\"profiles\":[" &&
                parse_shortcuts(read(path)) == model,
            "a hand edit that doesn't parse is kept as .bad, and the save goes ahead");
    // A store that never read the file finds it invalid.
    write(path, "not JSON");
    ShortcutStore fresh(path);
    require(!fresh.save(model) && read(bad) == "not JSON",
            "an invalid file under a fresh store goes to .bad");
    // A valid hand edit is kept as .old.
    require(store.load().state == ShortcutsLoad::State::Valid, "the saved file loads again");
    auto edited = model;
    edited.profiles[1].name = "By hand";
    write(path, serialize_shortcuts(edited));
    model.profiles[0].name = "Saved again";
    require(!store.save(model) && parse_shortcuts(read(old)) == edited &&
                parse_shortcuts(read(path)) == model,
            "a valid hand edit is kept as .old, and the save goes ahead");
    // What this store wrote last is replaced without a copy.
    fs::remove(old);
    model.profiles[0].name = "Third";
    require(!store.save(model) && !fs::exists(old), "its own last save is simply replaced");
    // R326: the folder's sync after the rename fails (a folder that can't be
    // opened for reading can still take the file). The new file is in place,
    // so the next save takes it for this store's own, not a hand edit.
    if (geteuid() != 0) {
        fs::permissions(directory.path, fs::perms::owner_write | fs::perms::owner_exec);
        model.profiles[0].name = "Unsynced";
        const auto problem = store.save(model);
        fs::permissions(directory.path, fs::perms::owner_all);
        require(problem && parse_shortcuts(read(path)) == model,
                "a failed folder sync is reported, the new file in place");
        model.profiles[0].name = "Fourth";
        require(!store.save(model) && !fs::exists(old) && parse_shortcuts(read(path)) == model,
                "and the next save replaces it without keeping it as .old");
    }
}

// Reload config rereads shortcuts.json: a valid hand edit shows, an invalid
// one keeps the bars already loaded and says so; --check reports the file.
void reload_and_check() {
    TemporaryDirectory directory;
    const auto path = shortcuts_path(directory.path);
    require(check_shortcuts(directory.path).ok &&
                check_shortcuts(directory.path).line == "shortcuts: none",
            "--check: no file is fine");
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    auto model = sample();
    model.free_profile = "b1";
    write(path, serialize_shortcuts(model));
    require(check_shortcuts(directory.path).line == "shortcuts: 2 profiles, 1 buttons, 0 assigned apps",
            "--check counts a valid file's bars, buttons and apps");
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    NullSink sink;
    App app(options, sink);
    const auto label = [&] {
        const auto view = app.view();
        return view.shortcut_bar && !view.shortcut_bar->buttons.empty()
                   ? view.shortcut_bar->buttons[0].label
                   : std::string("(none)");
    };
    require(label() == "Copy", "the bar loads");
    model.profiles[0].buttons[0].label = "Edited";
    write(path, serialize_shortcuts(model));
    app.reload();
    require(label() == "Edited", "Reload shows a valid hand edit");
    write(path, "{\"schema_version\":1,\"profiles\":[{\"id\":\"b1\",\"name\":\"SecretName\"}]}");
    app.reload();
    require(label() == "Edited" && app.view().status.find("can't be read") != std::string::npos &&
                app.view().status.find("SecretName") == std::string::npos,
            "Reload of an invalid file keeps the bars and says so, quoting nothing");
    const auto check = check_shortcuts(directory.path);
    require(!check.ok && check.line.starts_with("shortcuts: invalid: ") &&
                check.line.find("SecretName") == std::string::npos,
            "--check reports an invalid file, quoting nothing");
}
AppIdentity program(std::string id) {
    return {AppIdentity::Kind::Program, std::move(id)};
}
std::string shown(const ShortcutProfileChoice& choice) {
    return choice.shown() ? *choice.shown() : "";
}
void which_bar_shows() {
    ShortcutModel model;
    model.profiles = {{"editor", "Editor", {}}, {"free1", "Free 1", {}}, {"free2", "Free 2", {}}};
    model.apps = {{program("kate"), "Kate", "editor"}};
    model.free_profile = "free1";
    ShortcutProfileChoice choice;
    choice.configure(model, true, true);
    choice.focus(model, program("kate"));
    require(shown(choice) == "editor" && choice.button_on() && !choice.empty_bar(),
            "rule 1: the app's bar");
    choice.focus(model, program("other"));
    require(shown(choice) == "free1", "rule 2: the free bar chosen last");
    choice.focus(model, std::nullopt);
    require(shown(choice) == "free1", "Steam's dashboard and unknown apps count as apps without a bar");
    choice.focus(model, AppIdentity{AppIdentity::Kind::Steam, "620"});
    require(shown(choice) == "free1", "a Steam game without a bar");
    // The free bar chosen last is tied to an app, and nothing else was ever
    // picked: it shows all the same (R350).
    model.apps.push_back({program("term"), "Term", "free1"});
    choice.focus(model, program("other"));
    require(shown(choice) == "free1", "every pick assigned: the last pick all the same");
    choice.focus(model, program("term"));
    require(shown(choice) == "free1", "a later app with a bar brings the bar back");
    model.apps.pop_back();
    // The free bar no longer exists.
    model.free_profile = "gone";
    choice.focus(model, program("other"));
    require(!choice.shown(), "rule 2 needs the free bar to exist");
    model.free_profile.clear();
    choice.focus(model, program("other"));
    require(!choice.shown(), "rule 3 without a free bar");
    // Steam app identities are kinds of their own.
    model.apps.push_back({{AppIdentity::Kind::Steam, "620"}, "Game", "free2"});
    choice.focus(model, AppIdentity{AppIdentity::Kind::Steam, "620"});
    require(shown(choice) == "free2", "a Steam game's bar");
    choice.focus(model, program("620"));
    require(!choice.shown(), "a program of the same name is another app");
}
// R350: which profile an app without one shows, by the picks made for such
// apps, newest first; kept in shortcuts.json beside free_profile.
void pick_history() {
    using Ids = std::vector<std::string>;
    ShortcutModel model;
    model.profiles = {{"a", "A", {}}, {"b", "B", {}}, {"c", "C", {}}, {"d", "D", {}}};
    ShortcutProfileChoice choice;
    choice.configure(model, true, true);
    // 4: nothing ever picked: nothing.
    choice.focus(model, program("other"));
    require(!choice.shown(), "nothing picked ever: nothing shows");
    for (const char* id : {"a", "b", "c"}) {
        choice.focus(model, program("other"));
        choice.pick(model, id);
    }
    require(model.free_profile == "c" && model.earlier_picks == Ids{"b", "a"},
            "the picks, newest first");
    // 1: the last pick, no app assigned to it.
    choice.focus(model, program("new"));
    require(shown(choice) == "c", "the last pick while no app is assigned to it");
    // 2: it is assigned: the newest earlier pick that is not.
    model.apps.push_back({program("x"), "X", "c"});
    choice.focus(model, program("new"));
    require(shown(choice) == "b", "the last pick assigned: the newest earlier pick that is not");
    model.apps.push_back({program("y"), "Y", "b"});
    choice.focus(model, program("new"));
    require(shown(choice) == "a", "and the one before it");
    // A profile never picked is never chosen for the user.
    model.apps.push_back({program("z"), "Z", "a"});
    choice.focus(model, program("new"));
    // 3: every pick assigned: the last pick all the same; d, never picked, isn't chosen.
    require(shown(choice) == "c", "every pick assigned: the last pick, never one not picked");
    // Turned off by hand, nothing.
    choice.toolbar(model);
    choice.focus(model, program("new"));
    require(!choice.shown(), "the switch off by hand overrides it all");
    choice.toolbar(model);
    // Deleting a profile drops it; the next takes the last pick's place.
    model.apps.clear();
    std::erase_if(model.profiles, [](const ShortcutProfile& p) { return p.id == "c"; });
    auto changes = choice.model_changed(model);
    require(changes.free_profile && model.free_profile == "b" && model.earlier_picks == Ids{"a"},
            "a deleted profile leaves the picks");
    std::erase_if(model.profiles, [](const ShortcutProfile& p) { return p.id == "a"; });
    require(model.forget_missing_picks() && model.free_profile == "b" && model.earlier_picks.empty(),
            "an earlier one too");
    // Picked again, a profile moves to the front, once.
    model.profiles.push_back({"a", "A", {}});
    model.remember_pick("a");
    model.remember_pick("d");
    model.remember_pick("b");
    require(model.free_profile == "b" && model.earlier_picks == Ids{"d", "a"},
            "a pick again moves to the front");
    accepted(model, "the picks");
    // Bounded by the profile limit.
    ShortcutModel many;
    for (std::size_t i = 0; i < shortcut_limits::profiles; ++i) {
        many.profiles.push_back({"p" + std::to_string(i), "P", {}});
        many.remember_pick("p" + std::to_string(i));
    }
    many.remember_pick("p0");
    require(many.free_profile == "p0" && many.earlier_picks.size() == shortcut_limits::profiles - 1 &&
                std::find(many.earlier_picks.begin(), many.earlier_picks.end(), "p0") ==
                    many.earlier_picks.end(),
            "at most one pick per profile");
    accepted(many, "picks of every profile");
    // Bad picks make the file invalid, as a bad free_profile does.
    auto bad = model;
    bad.earlier_picks.push_back("gone");
    refused(bad, "an earlier pick of no profile");
    bad = model;
    bad.earlier_picks.push_back("d");
    refused(bad, "a profile picked twice");
    bad = model;
    bad.earlier_picks.push_back("b");
    refused(bad, "the last pick also earlier");
    bad = model;
    bad.free_profile.clear();
    refused(bad, "earlier picks without a last one");
    // A file from before keeps its free_profile as a history of one.
    const auto old = parse_shortcuts(
        R"({"schema_version": 1, "profiles": [{"id": "a", "name": "A", "buttons": []}], "free_profile": "a"})");
    require(old.free_profile == "a" && old.earlier_picks.empty(), "an old file: a history of one");
}
void toolbar_button() {
    ShortcutModel model;
    model.profiles = {{"editor", "Editor", {}}, {"free1", "Free 1", {}}};
    model.apps = {{program("kate"), "Kate", "editor"}};
    ShortcutProfileChoice choice;
    choice.configure(model, true, true);
    choice.focus(model, program("kate"));
    // A bar shows: it hides and stays hidden everywhere.
    auto changes = choice.toolbar(model);
    require(!choice.shown() && !choice.button_on() && !choice.switch_on() && changes.shortcuts_switch &&
                !changes.free_profile,
            "pressed while a bar shows: hidden, and the switch is saved off");
    choice.focus(model, program("other"));
    choice.focus(model, program("kate"));
    require(!choice.shown() && !choice.button_on(), "turned off by hand overrides every rule");
    // Nothing shows and a bar is due: that bar.
    changes = choice.toolbar(model);
    require(shown(choice) == "editor" && choice.switch_on() && changes.shortcuts_switch,
            "pressed while nothing shows and a bar is due: that bar");
    // Nothing shows and none is due: the empty bar, without a switch change.
    choice.focus(model, program("other"));
    require(!choice.shown() && !choice.button_on() && choice.switch_on(), "rule 3 shows nothing");
    changes = choice.toolbar(model);
    require(!choice.shown() && choice.empty_bar() && choice.button_on() && !changes.shortcuts_switch,
            "pressed while no bar is due: the empty two-slot bar");
    choice.focus(model, program("other"));
    require(!choice.empty_bar() && !choice.button_on(), "the empty bar stays until the focus moves on");
    // The empty bar hides like any bar.
    choice.toolbar(model);
    changes = choice.toolbar(model);
    require(!choice.empty_bar() && !choice.switch_on() && changes.shortcuts_switch,
            "pressed while the empty bar shows: hidden and the switch off");
    // Switched off with no bar due, a press shows the empty bar.
    changes = choice.toolbar(model);
    require(choice.empty_bar() && choice.switch_on() && changes.shortcuts_switch,
            "switched back on with no bar due: the empty bar");
    // The saved switch is honored at start.
    ShortcutProfileChoice later;
    later.configure(model, true, false);
    later.focus(model, program("kate"));
    require(!later.shown() && !later.switch_on(), "a saved switch that is off shows nothing");
    // With the feature off nothing shows and the button does nothing.
    ShortcutProfileChoice off;
    off.configure(model, false, true);
    off.focus(model, program("kate"));
    changes = off.toolbar(model);
    require(!off.shown() && !off.empty_bar() && !changes.shortcuts_switch && off.switch_on(),
            "Shortcuts and macros off: nothing shows");
    off.configure(model, true, true);
    require(shown(off) == "editor", "turned on: the focused app's bar shows");
}
void picker() {
    ShortcutModel model;
    model.profiles = {{"editor", "Editor", {}}, {"free1", "Free 1", {}}, {"free2", "Free 2", {}}};
    model.apps = {{program("kate"), "Kate", "editor"}};
    model.free_profile = "free1";
    ShortcutProfileChoice choice;
    choice.configure(model, true, true);
    choice.focus(model, program("kate"));
    // Picked while an app with its own bar has the keys: it shows, and
    // stays temporary (R350).
    auto changes = choice.pick(model, "free2");
    require(shown(choice) == "free2" && model.free_profile == "free1" && !changes.free_profile,
            "a pick for an app with its own bar is not remembered");
    // Turned off and on in the same focus, the pick is still what is due.
    choice.toolbar(model);
    choice.toolbar(model);
    require(shown(choice) == "free2", "the picked bar is due until the focus changes");
    choice.focus(model, program("kate"));
    require(shown(choice) == "editor", "the pick lasts until the focus changes");
    choice.focus(model, program("other"));
    require(shown(choice) == "free1", "rule 2 returns to the bar picked last for apps without one");
    // Picked while an app without a bar has the keys: first in the picks.
    changes = choice.pick(model, "free2");
    require(shown(choice) == "free2" && model.free_profile == "free2" &&
                model.earlier_picks == std::vector<std::string>{"free1"} && changes.free_profile,
            "a pick for an app without a bar goes first in the picks");
    changes = choice.pick(model, "editor");
    require(shown(choice) == "editor" && model.free_profile == "editor" &&
                model.earlier_picks == std::vector<std::string>{"free2", "free1"} &&
                changes.free_profile,
            "an assigned bar picked for such an app is remembered too");
    changes = choice.pick(model, "editor");
    require(shown(choice) == "editor" && !changes.free_profile,
            "picking the last pick again saves nothing");
    // Picking from the empty bar.
    model.free_profile.clear();
    model.earlier_picks.clear();
    choice.focus(model, program("other"));
    choice.toolbar(model);
    require(choice.empty_bar(), "the empty bar");
    changes = choice.pick(model, "free1");
    require(shown(choice) == "free1" && !choice.empty_bar() && model.free_profile == "free1" &&
                changes.free_profile,
            "a bar picked from the empty bar");
    // Nothing is picked while the bar is off by hand, or for a bar that doesn't exist.
    choice.toolbar(model);
    changes = choice.pick(model, "free2");
    require(!choice.shown() && model.free_profile == "free1" && !changes.free_profile,
            "nothing is picked while the switch is off");
    choice.toolbar(model);
    changes = choice.pick(model, "gone");
    require(shown(choice) == "free1" && !changes.free_profile, "a bar that doesn't exist is not picked");
}
void model_changes() {
    ShortcutModel model;
    model.profiles = {{"editor", "Editor", {}}, {"free1", "Free 1", {}}};
    model.apps = {{program("kate"), "Kate", "editor"}};
    model.free_profile = "free1";
    ShortcutProfileChoice choice;
    choice.configure(model, true, true);
    choice.focus(model, program("other"));
    require(shown(choice) == "free1", "the free bar");
    // Assigning the focused app to another bar doesn't switch the bar showing.
    model.apps.push_back({program("other"), "Other", "editor"});
    auto changes = choice.model_changed(model);
    require(shown(choice) == "free1" && !changes.free_profile, "a new tie leaves the bar showing");
    // The bar showing and the free bar are deleted.
    model.profiles.erase(model.profiles.begin() + 1);
    changes = choice.model_changed(model);
    require(model.free_profile.empty() && changes.free_profile, "a free bar that is gone is cleared");
    require(shown(choice) == "editor", "a deleted bar gives way to the bar now due");
    // Nothing showing may now have a bar due.
    model.apps.clear();
    choice.focus(model, program("other"));
    require(!choice.shown(), "nothing due");
    model.apps.push_back({program("other"), "Other", "editor"});
    choice.model_changed(model);
    require(shown(choice) == "editor", "a tie made in the editor shows the bar now due");
    // The empty bar stays through a model change.
    model.apps.clear();
    choice.focus(model, program("other"));
    choice.toolbar(model);
    choice.model_changed(model);
    require(choice.empty_bar(), "the empty bar stays");
}
// Ties made in the picker: one per app, moved by a re-tie, at most 128.
void ties() {
    ShortcutModel model;
    model.profiles = {{"p1", "One", {}}, {"p2", "Two", {}}};
    require(model.assign(program("kate"), "Kate", "p1") &&
                model.profile_for(program("kate"))->id == "p1",
            "a tie");
    require(model.assign(program("kate"), "Kate", "p2") && model.apps.size() == 1 &&
                model.profile_for(program("kate"))->id == "p2" && model.free("p1"),
            "a re-tie clears the app's other tie");
    require(!model.assign(program("kate"), "Kate", "gone") &&
                model.tie(program("kate"))->profile == "p2",
            "no tie to a profile that doesn't exist");
    require(model.unassign(program("kate")) && model.apps.empty() && !model.unassign(program("kate")),
            "an untie, once");
    for (std::size_t i = 0; i < shortcut_limits::apps; ++i) {
        require(model.assign(program("app" + std::to_string(i)), "App", i % 2 ? "p1" : "p2"),
                "ties up to the limit");
    }
    require(!validate_shortcuts(model), "128 ties are a valid file");
    require(!model.can_assign(program("more")) && !model.assign(program("more"), "More", "p1") &&
                model.apps.size() == shortcut_limits::apps,
            "no 129th app");
    require(model.can_assign(program("app0")) && model.assign(program("app0"), "App", "p1") &&
                model.tie(program("app0"))->profile == "p1",
            "a tied app's tie still moves at the limit");
}
// A stream from a PC (R359): the streamed game's app ID from the streaming
// client's arguments, strictly, and the PC's stream for anything else. The
// lists here stand for /proc; no test reads it. The account and address
// arguments are made up and never part of the answer.
void stream_apps() {
    const std::vector<std::string> base{"/usr/bin/streaming_client", "--account", "12345", "--server",
                                        "192.0.2.7:27036"};
    const auto with = [&](std::vector<std::string> more) {
        auto arguments = base;
        arguments.insert(arguments.end(), more.begin(), more.end());
        return arguments;
    };
    require(streamed_app_id(with({"--gameid", "4000020", "--appid", "4000020"})) == 4000020u,
            "--appid and its number");
    require(!streamed_app_id(base), "no --appid");
    require(!streamed_app_id(with({"--appid", "0"})), "0 is no app");
    require(!streamed_app_id(with({"--appid", "4294967296"})) &&
                streamed_app_id(with({"--appid", "4294967295"})) == 4294967295u,
            "the ID fits 32 bits");
    require(!streamed_app_id(with({"--appid", "12345678901"})), "too many digits");
    for (const char* odd : {"4000020x", "-1", "+4", " 4", "", "0400", "4e6"}) {
        require(!streamed_app_id(with({"--appid", odd})), std::string("not a plain number: ") + odd);
    }
    require(!streamed_app_id(with({"--appid"})), "--appid last, with nothing after it");
    require(!streamed_app_id(with({"--appid=4000020"})) && !streamed_app_id(with({"--APPID", "4"})),
            "only the argument exactly --appid");
    require(!streamed_app_id(with({"--appid", "4000020", "--appid", "4000030"})),
            "two --appid: a surprise");
    require(!streamed_app_id(with({"--gameid", "4000020"})), "--gameid alone is not read");

    using Clients = std::optional<std::vector<std::vector<std::string>>>;
    const AppIdentity game{AppIdentity::Kind::Steam, "4000020"};
    require(stream_identity(Clients{{with({"--appid", "4000020"})}}) == game,
            "one client with an ID: the Steam game");
    require(stream_identity(Clients{{base}}) == pc_stream_identity(), "one without: the PC's stream");
    require(stream_identity(std::nullopt) == pc_stream_identity(), "unreadable: the PC's stream");
    require(stream_identity(Clients{std::vector<std::vector<std::string>>{}}) == pc_stream_identity(),
            "no client: the PC's stream");
    require(stream_identity(Clients{{with({"--appid", "4000020"}), with({"--appid", "4000020"})}}) ==
                pc_stream_identity(),
            "two clients: the PC's stream, never a guess");
    require(app_identity_valid(game) && app_identity_valid(pc_stream_identity()) &&
                !app_identity_valid({AppIdentity::Kind::Stream, "desktop"}),
            "a stream's one ID is pc");

    // The finder answers each request with its number, off the caller's thread.
    std::atomic<int> reads{};
    StreamAppFinder finder([&]() -> Clients {
        ++reads;
        return Clients{{with({"--appid", "4000020"})}};
    });
    finder.request(7);
    std::optional<StreamAppFinder::Result> answer;
    for (int i = 0; i < 1000 && !answer; ++i) {
        usleep(1000);
        answer = finder.take();
    }
    require(answer && answer->session == 7 && answer->app == game && reads == 1,
            "an answer carries its session's number");
    StreamAppFinder none({});
    none.request(3);
    const auto at_once = none.take();
    require(at_once && at_once->session == 3 && at_once->app == pc_stream_identity(),
            "without a source nothing is read: the PC's stream, at once");

    // The PC's stream in shortcuts.json: kind "stream", the one ID.
    ShortcutModel model;
    model.profiles = {{"p1", "One", {}}};
    model.apps = {{pc_stream_identity(), "PC desktop stream", "p1"}, {game, "App 4000020", "p1"}};
    accepted(model, "a stream tie");
    require(serialize_shortcuts(model).find("\"kind\": \"stream\"") != std::string::npos ||
                serialize_shortcuts(model).find("\"kind\":\"stream\"") != std::string::npos,
            "written as kind stream");
    model.apps[0].app.id = "desktop";
    refused(model, "a stream with another ID");
}
// App names, from a temporary folder only: never the computer's own files.
void app_names() {
    TemporaryDirectory folder;
    const auto steam = folder.path / "Steam", second = folder.path / "card";
    fs::create_directories(steam / "steamapps");
    fs::create_directories(second / "steamapps");
    write(steam / "steamapps" / "libraryfolders.vdf",
          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam.string() +
              "\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"4000010\"\t\t\"1\"\n\t\t}\n\t}\n"
              "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
              second.string() + "\"\n\t}\n}\n");
    write(second / "steamapps" / "appmanifest_4000020.acf",
          "\"AppState\"\n{\n\t\"appid\"\t\t\"4000020\"\n\t\"name\"\t\t\"Second \\\"Library\\\" Game\"\n"
          "\t\"UserConfig\"\n\t{\n\t\t\"name\"\t\t\"wrong\"\n\t}\n}\n");
    // A manifest whose name lies past the cutoff, and one with bad UTF-8.
    write(steam / "steamapps" / "appmanifest_4000030.acf", "\"AppState\"\n{\n\t\"padding\"\t\t\"" +
                                                               std::string(app_name_file_bytes, 'x') +
                                                               "\"\n\t\"name\"\t\t\"Too Far\"\n}\n");
    write(steam / "steamapps" / "appmanifest_4000040.acf",
          "\"AppState\"\n{\n\t\"name\"\t\t\"Bad \xff Name\"\n}\n");
    write(steam / "steamapps" / "appmanifest_4000050.acf",
          "\"AppState\"\n{\n\t\"name\"\t\t\"  Long\x01 " + std::string(200, 'n') + "\"\n}\n");
    const auto applications = folder.path / "applications", system = folder.path / "system";
    fs::create_directories(applications);
    fs::create_directories(system);
    write(system / "org.example.Notes.desktop",
          "[Desktop Action new]\nName=Wrong\n[Desktop Entry]\nType=Application\nName[de]=Notizen\n"
          "Name=Example\\sNotes\nExec=notes\n");
    write(applications / "org.example.Empty.desktop", "[Desktop Entry]\nType=Application\n");
    AppNameFolders folders{{steam}, {applications, system}};
    const auto steam_app = [](std::string id) {
        return AppIdentity{AppIdentity::Kind::Steam, std::move(id)};
    };
    const auto desktop = [](std::string id) {
        return AppIdentity{AppIdentity::Kind::Desktop, std::move(id)};
    };
    require(look_up_app_name(steam_app("4000020"), folders) == "Second \"Library\" Game",
            "a manifest found in the second library libraryfolders.vdf lists, by AppState's own name");
    require(look_up_app_name(steam_app("4000010"), folders).empty(), "a manifest that is missing: none");
    require(look_up_app_name(steam_app("4000030"), folders).empty(), "a name past the cutoff: none");
    require(look_up_app_name(steam_app("4000040"), folders).empty(), "a name that isn't UTF-8: none");
    const auto long_name = look_up_app_name(steam_app("4000050"), folders);
    require(long_name.starts_with("Long n") &&
                g_utf8_strlen(long_name.c_str(), -1) ==
                    static_cast<glong>(shortcut_limits::app_name_characters),
            "a name without its control characters and spaces, cut to 128 characters");
    require(look_up_app_name(desktop("org.example.Notes"), folders) == "Example Notes",
            "Name= of the desktop file's own group, from the second folder");
    require(look_up_app_name(desktop("org.example.Empty"), folders).empty() &&
                look_up_app_name(desktop("org.example.Missing"), folders).empty(),
            "a desktop file without Name=, or missing: none");
    require(look_up_app_name(desktop("../system/org.example.Notes"), folders).empty(),
            "a Desktop app ID is a file name, never a path");
    require(look_up_app_name(program("notes"), folders).empty(), "a program's name is its file name");
    require(
        fallback_app_name(program("moonlight")) == "moonlight" &&
            fallback_app_name(desktop("org.example.Notes")) == "Notes" &&
            fallback_app_name(desktop("notes")) == "notes" &&
            fallback_app_name(steam_app("4000010"), "game.x86_64") == "game.x86_64" &&
            fallback_app_name(steam_app("4000010")) == "App 4000010",
        "until a name is found: the file name, the ID's last part, the program, else App and the ID");
    // R346: Wine names a game's process by its Windows path.
    require(fallback_app_name(program("C:\\Games\\Example\\game.exe")) == "game.exe" &&
                fallback_app_name(steam_app("4000010"), "Z:\\home\\games\\example.exe") ==
                    "example.exe" &&
                fallback_app_name(program("mixed/dir\\tool.exe")) == "tool.exe",
            "a Windows path names the program by what follows its last separator");
    const auto standard = system_app_name_folders("/home/someone");
    require(standard.steam.front() == "/home/someone/.local/share/Steam" &&
                std::find(standard.applications.begin(), standard.applications.end(),
                          "/var/lib/flatpak/exports/share/applications") !=
                    standard.applications.end() &&
                std::find(standard.applications.begin(), standard.applications.end(),
                          "/home/someone/.local/share/flatpak/exports/share/applications") !=
                    standard.applications.end(),
            "the standard folders, Flatpak's exports included");
    // The lookup's own thread: a result taken later, never a repeat.
    std::atomic<int> calls{};
    AppNameLookup lookup([&](const AppIdentity& app) {
        ++calls;
        return look_up_app_name(app, folders);
    });
    lookup.request(desktop("org.example.Notes"));
    std::optional<AppNameLookup::Result> result;
    for (int i = 0; i < 500 && !result; ++i) {
        result = lookup.take();
        if (!result) {
            usleep(2000);
        }
    }
    require(result && result->app == desktop("org.example.Notes") && result->name == "Example Notes",
            "a lookup's result is taken later");
    AppNameLookup none{AppNameSource{}};
    none.request(program("notes"));
    const auto answered = none.take();
    require(answered && answered->name.empty(), "without a source, an answer at once, with no name");
}
// Run in the editor's copy: Script and Command each keep their content while
// the editor is open; only the chosen one is saved.
void run_editing() {
    ShortcutModel model;
    ShortcutButton button;
    button.label = "Go";
    button.action.chords = {{{"ControlLeft"}, "KeyG"}};
    model.profiles.push_back({"p", "P", {button}});
    ShortcutEditor editor(model, nullptr);
    editor.set_kind(ShortcutAction::Kind::Run);
    const auto action = [&] { return editor.selected_button()->action; };
    require(action().kind == ShortcutAction::Kind::Run && !action().command_chosen &&
                action().chords.size() == 1,
            "Run is chosen, Script first; the shortcut's keys are kept");
    editor.set_script("tool.sh");
    editor.set_script("../tool.sh");
    require(action().script == "tool.sh", "a script name with a folder part is refused");
    editor.set_run_command(true);
    editor.set_command(std::string(512, 'a'));
    require(action().command.size() == 512, "a command of 512 bytes");
    editor.set_command(std::string(513, 'b'));
    editor.set_command("two\nlines");
    require(action().command == std::string(512, 'a'), "longer or two lines: refused");
    editor.set_command("echo hi");
    require(action().script == "tool.sh" && action().command == "echo hi",
            "Script and Command each keep theirs");
    auto saved = editor.saved();
    auto run = saved.profiles[0].buttons[0].action;
    require(run.kind == ShortcutAction::Kind::Run && run.command == "echo hi" && run.script.empty() &&
                run.chords.empty() && !validate_shortcuts(saved),
            "the command chosen is saved alone");
    editor.set_run_command(false);
    run = editor.saved().profiles[0].buttons[0].action;
    require(run.script == "tool.sh" && run.command.empty(), "the script chosen is saved alone");
    editor.set_kind(ShortcutAction::Kind::Keys);
    require(editor.saved().profiles[0].buttons[0].action.chords.size() == 1 &&
                editor.saved().profiles[0].buttons[0].action.script.empty(),
            "back to Shortcut: its keys, no script");
    require(fits_command("ls -la ~") && !fits_command("a\tb") && !fits_command(std::string("bad\xff")),
            "a command is one line of valid text");
    // A Run button read from the file shows the kind it holds.
    model.profiles[0].buttons[0].action = {};
    model.profiles[0].buttons[0].action.kind = ShortcutAction::Kind::Run;
    model.profiles[0].buttons[0].action.command = "true";
    ShortcutEditor opened(model, nullptr);
    require(opened.selected_button()->action.command_chosen && !opened.changed(),
            "a command opens on Command, unchanged");
}
} // namespace

int main() {
    try {
        example_file();
        empty_actions();
        invalid_files();
        limits();
        store();
        bad_file_rule();
        hand_edits();
        reload_and_check();
        which_bar_shows();
        pick_history();
        toolbar_button();
        picker();
        model_changes();
        ties();
        stream_apps();
        run_editing();
        app_names();
        std::cout << "Shortcut file, limits, saving and which bar shows passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

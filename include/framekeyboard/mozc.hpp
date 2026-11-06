#pragma once
// Kanji conversion by Mozc, the open-source engine of Google Japanese Input,
// run as a server process of Full Keyboard's own (docs/japanese.md, Mozc).
// Our own layer (usual forms, the phrase list) works on its segments as on
// Anthy's, and Anthy converts whenever Mozc can't, as while it starts.
#include "framekeyboard/japanese.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace framekeyboard {
// The Mozc server shipped beside the keyboard, framekeyboard-mozc: in
// engines/ in a development build, lib/framekeyboard/ in a release. Empty
// when there is none.
std::filesystem::path mozc_engine();
// Starts the server at `path`, once per process, with its user profile in
// `profile`/mozc, apart from any Mozc the user runs. It doesn't wait: Mozc
// answers in a moment, and Anthy converts until then. It is kept running,
// and started again if it crashes, until stop_mozc(); one that keeps
// crashing as it starts is left stopped until it is started after that.
// Calling this again while it runs starts it again if it went. `words`, a
// reading<TAB>word list such as japanese/mozc-words.tsv, becomes its user
// dictionary.
//
// The keyboard makes these calls, and any conversion, on its conversion
// worker (JapaneseWorker): they talk to the server and wait for processes.
// The VR thread calls only mozc_needs_start(), mozc_unsaved_learning() and
// mozc_learned(), which do neither.
void start_mozc(const std::filesystem::path& path, const std::filesystem::path& profile,
                const std::filesystem::path& words = {});
// Waits up to `patience` for the server to answer; false if it doesn't.
bool wait_for_mozc(std::chrono::milliseconds patience);
// Stops the server, saving what it learned; it isn't started again. One
// that doesn't go within a second of being asked is killed.
void stop_mozc();
bool mozc_running();
// Whether start_mozc() has work: the server is wanted, but it went, or it
// waits to be started again and may be by now. Without locks or waiting,
// for the VR thread's periodic check.
bool mozc_needs_start();
// Mozc doesn't save a phrase it learns at once, but every few minutes on its
// own. Until it is saved, here a count that grows with each one; 0 once
// saved, or when nothing was learned. Without locks, for the VR thread.
std::uint64_t mozc_unsaved_learning();
// Saves it on a thread of its own, so a slow save holds up no conversion
// or frame; nothing without anything to save.
void save_mozc_learning();
// Whether Mozc learned a phrase in `profile`, and forgetting all it learned:
// Mozc is asked to, or, one that doesn't answer stopped, its profile is
// removed, to start over. False if that failed, still learned.
bool mozc_learned(const std::filesystem::path& profile);
bool clear_mozc_learning(const std::filesystem::path& profile);
// A converter using that server, and `fallback`'s converter, made on first
// need, for any conversion Mozc can't make. With `patience`, as the
// simulator and its candidates report have, a conversion Mozc can't make
// waits that long for the server, started again if it went, before the
// fallback converts; the keyboard doesn't wait.
std::unique_ptr<JapaneseConverter>
mozc_converter(std::function<std::unique_ptr<JapaneseConverter>()> fallback,
               std::chrono::milliseconds patience = {});
} // namespace framekeyboard

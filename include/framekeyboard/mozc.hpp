#pragma once
// Kanji conversion by Mozc, the open-source engine of Google Japanese Input,
// run as a server process of Full Keyboard's own (docs/japanese.md, Mozc).
// Our own layer (usual forms, the phrase list) works on its segments as on
// Anthy's, and Anthy converts whenever Mozc can't, as while it starts.
#include "framekeyboard/japanese.hpp"
#include <chrono>
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
void start_mozc(const std::filesystem::path& path, const std::filesystem::path& profile,
                const std::filesystem::path& words = {});
// Waits up to `patience` for the server to answer; false if it doesn't.
bool wait_for_mozc(std::chrono::milliseconds patience);
// Stops the server, saving what it learned; it isn't started again.
void stop_mozc();
bool mozc_running();
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

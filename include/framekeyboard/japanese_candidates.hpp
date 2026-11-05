#pragma once
// What Japanese input offers for a list of phrases, without a keyboard: the
// live candidates and 変換's segments for each, to check conversion quickly
// (docs/simulator.md, Japanese candidates). Learning stays off, so each
// run offers what a new profile does.
#include "framekeyboard/japanese.hpp"
#include <cstddef>
#include <filesystem>
#include <iosfwd>

namespace framekeyboard {
struct JapaneseCandidatesOptions {
    std::filesystem::path data_dir;      // An extra profile folder, as the keyboard's.
    std::size_t live = 12;               // Live candidates listed for each phrase.
    std::size_t segment = 6;             // Candidates listed for each of 変換's segments.
    JapaneseComposer::Factory converter; // Anthy unless set.
    // Mozc instead, Anthy only while Mozc can't convert: its server and a
    // folder for its profile (mozc.hpp).
    std::filesystem::path mozc_server, mozc_profile;
    bool word_forms = true; // JapaneseComposer::set_word_forms()
};
// Reads phrases, one a line, and writes what Japanese input offers for each.
// A line with Latin letters is typed as romaji, any other as kana; spaces
// are left out. Empty lines and lines starting with # are copied as they are.
void report_japanese_candidates(std::istream& phrases, std::ostream& report,
                                const JapaneseCandidatesOptions& options = {});
} // namespace framekeyboard

#pragma once
// What Pinyin and Zhuyin offer for a list of readings, without a keyboard: the
// conversion and the candidates for its start, to compare the engines quickly
// (docs/simulator.md, Chinese candidates). Nothing is learned.
#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace framekeyboard {
struct ChineseCandidatesOptions {
    // Input methods to type each line with; empty picks by the line: Zhuyin
    // for Zhuyin symbols, both Pinyin methods for Latin letters.
    std::vector<std::string> methods;
    // The engines to compare (pyzy, libime, mcbopomofo), each where
    // it has the method; empty is the one the keyboard uses.
    std::vector<std::string> engines;
    std::size_t count = 12; // Candidates listed for each line.
};
// Reads readings, one a line, and writes what each method offers for them.
// Pinyin leaves out spaces and tone digits. Zhuyin symbols are typed on the
// standard Zhuyin keys: a space gives a syllable without a tone mark the
// first tone, as one left open at the end gets, and after a tone mark only
// separates. With a Zhuyin method named, a Latin line is those keys themselves;
// with Cangjie or Quick named (chinese-cangjie, chinese-quick), it is their keys.
// Empty lines and lines starting with # are copied as they are.
void report_chinese_candidates(std::istream& readings, std::ostream& report,
                               const ChineseCandidatesOptions& options = {});
} // namespace framekeyboard

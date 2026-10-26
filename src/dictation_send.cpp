#include "framekeyboard/dictation_send.hpp"

#include <glib.h>

namespace framekeyboard {
namespace {
constexpr std::size_t commit_limit = 32; // the output queue's commit size, in code points

// Cuts text bytes [begin, end) into commits on character boundaries.
void add_text(std::vector<SendPiece>& pieces, const std::string& text, std::size_t begin,
              std::size_t end) {
    while (begin < end) {
        std::size_t stop = begin;
        for (std::size_t count = 0; count < commit_limit && stop < end; ++count) {
            stop = static_cast<std::size_t>(g_utf8_next_char(text.c_str() + stop) - text.c_str());
        }
        pieces.push_back({SendPiece::Kind::Text, text.substr(begin, stop - begin), begin, stop});
        begin = stop;
    }
}
} // namespace

std::vector<SendPiece> send_pieces(const std::string& text, bool line_breaks) {
    const auto first = text.find_first_not_of(" \n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \n") + 1;
    std::vector<SendPiece> pieces;
    std::size_t start = first;
    std::size_t i = first;
    while (i < last) {
        if (text[i] != '\n' && text[i] != ' ') {
            ++i;
            continue;
        }
        // A run of spaces and line breaks; only one with a break changes.
        std::size_t run = i;
        while (run < last && (text[run] == ' ' || text[run] == '\n')) {
            ++run;
        }
        const auto breaks = text.find('\n', i);
        if (breaks >= run) {
            i = run;
            continue;
        }
        if (line_breaks) {
            add_text(pieces, text, start, breaks);
            for (std::size_t b = breaks; b < run && text[b] == '\n'; ++b) {
                pieces.push_back({SendPiece::Kind::Break, {}, b, b + 1});
                start = b + 1;
            }
            i = start; // spaces after the breaks start the next text
            continue;
        }
        add_text(pieces, text, start, i);
        pieces.push_back({SendPiece::Kind::Space, " ", i, run});
        start = i = run;
    }
    add_text(pieces, text, start, last);
    pieces.front().begin = 0; // it also covers the dropped spaces and breaks
    pieces.push_back({SendPiece::Kind::Enter, {}, last, text.size()});
    return pieces;
}
} // namespace framekeyboard

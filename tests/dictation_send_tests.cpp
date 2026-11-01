#include "framekeyboard/dictation_send.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace framekeyboard;
namespace {
using Kind = SendPiece::Kind;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
// One letter per piece: T text, S space, B break.
std::string shape(const std::vector<SendPiece>& pieces) {
    std::string result;
    for (const auto& piece : pieces) {
        result += piece.kind == Kind::Text ? 'T' : piece.kind == Kind::Space ? 'S' : 'B';
    }
    return result;
}
// The pieces cover the text up to its last character in order, without gaps
// or overlaps, and text pieces are their source bytes.
void require_cover(const std::string& text, const std::vector<SendPiece>& pieces) {
    std::size_t at = 0;
    for (const auto& piece : pieces) {
        require(piece.begin == at && piece.end >= piece.begin, "pieces are contiguous");
        if (piece.kind == Kind::Text) {
            require(text.substr(piece.begin, piece.end - piece.begin).ends_with(piece.text),
                    "a text piece is its source");
        }
        at = piece.end;
    }
    require(at == text.find_last_not_of(" \n") + 1, "the pieces end with the last character");
}

void plain() {
    require(send_pieces("", true).empty() && send_pieces(" \n \n", true).empty(),
            "nothing to send without text");
    const auto pieces = send_pieces("Hello there.", true);
    require(shape(pieces) == "T" && pieces[0].text == "Hello there.", "one commit, no Enter");
    require_cover("Hello there.", pieces);
}

void chunks() {
    std::string text;
    for (int i = 0; i < 40; ++i) {
        text += "é"; // two bytes each: chunks count code points
    }
    const auto pieces = send_pieces(text, true);
    require(shape(pieces) == "TT" && pieces[0].text.size() == 64 && pieces[1].text.size() == 16,
            "commits hold at most 32 code points");
    require_cover(text, pieces);
}

void line_breaks() {
    const std::string text = "\n\n  One\ntwo  \n\nthree \n\n";
    const auto pieces = send_pieces(text, true);
    require(shape(pieces) == "TBTBBT", "each inner line break is Shift+Enter");
    require(pieces[0].text == "One" && pieces[0].begin == 0, "leading breaks and spaces are dropped");
    require(pieces[2].text == "two  " && pieces[5].text == "three", "trailing spaces and breaks too");
    require_cover(text, pieces);

    const auto spaced = send_pieces("a \n b", true);
    require(shape(spaced) == "TBT" && spaced[0].text == "a " && spaced[2].text == " b",
            "spaces next to a break stay where the target takes line breaks");
}

void single_line() {
    const std::string text = "a\n\n\nb  \n c d";
    const auto pieces = send_pieces(text, false);
    require(shape(pieces) == "TSTST", "a run of breaks becomes one space");
    require(pieces[1].text == " " && pieces[1].begin == 1 && pieces[1].end == 4,
            "the space covers the whole run");
    require(pieces[2].text == "b" && pieces[3].end == text.find('c'),
            "the spaces around a break join its run");
    require(pieces[4].text == "c d", "spaces without a break are ordinary text");
    require_cover(text, pieces);
}
void paste() {
    const std::string text = " \nOne\n\ntwo \n";
    const auto pieces = send_pieces(text, true);
    const auto pasted = paste_pieces(text);
    require(pasted.size() == 1 && pasted[0].kind == Kind::Paste && pasted[0].text == "One\n\ntwo" &&
                pasted[0].begin == 0,
            "a paste is the trimmed text in one piece");
    require(pieces.back().end == pasted[0].end, "both end where the trimmed text ends");
    require(paste_pieces(" \n").empty(), "nothing to paste without text");
}
void commits() {
    std::string text = "ab";
    for (int i = 0; i < 40; ++i) {
        text += "日";
    }
    const auto pieces = text_commits(text);
    require(pieces.size() == 2 && pieces[0].size() == 2 + 30 * 3 && pieces[1] == "日日日日日日日日日日",
            "commits hold at most 32 code points, cut between characters");
    require(text_commits("").empty() && text_commits("short") == std::vector<std::string>{"short"},
            "short text is one commit");
}
} // namespace

int main() {
    try {
        plain();
        chunks();
        line_breaks();
        single_line();
        paste();
        commits();
        std::cout << "Dictation send tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

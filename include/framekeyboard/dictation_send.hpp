#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace framekeyboard {
// One delivery of Send (docs/dictation.md, Send), in the order sent. Each
// piece covers the editor bytes [begin, end); once it is delivered, those
// bytes leave the editor. The pieces together cover the whole text.
struct SendPiece {
    enum class Kind {
        Text,  // `text`, at most 32 code points, the same as its source bytes
        Space, // a single-line field's stand-in for a run of line breaks
        Break, // Shift+Enter for one line break
        Enter, // the final plain Enter, covering any trailing line breaks
        Paste, // `text` through the clipboard and Ctrl+V, line breaks and all
    };
    Kind kind{};
    std::string text; // Text, Space and Paste only
    std::size_t begin{}, end{};
    bool operator==(const SendPiece&) const = default;
};
// Spaces and line breaks at the start and end are dropped. Between text, line
// breaks become Shift+Enter, or, where the target takes only one line, each
// run of them with the spaces around it becomes one space. Empty when there
// is no text.
std::vector<SendPiece> send_pieces(const std::string& text, bool line_breaks);
// The same text pasted in one piece, then the final Enter.
std::vector<SendPiece> paste_pieces(const std::string& text);
} // namespace framekeyboard

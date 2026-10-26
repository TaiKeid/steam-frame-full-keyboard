#include "framekeyboard/dictation_editor.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void typing_and_cursor() {
    DictationEditor editor;
    require(editor.type("héllo") && editor.text() == "héllo" && editor.length() == 5 &&
                editor.cursor() == 6,
            "typing counts code points and keeps the cursor after the text");
    editor.left();
    editor.left();
    editor.left();
    require(editor.cursor() == 3, "left steps over whole characters");
    editor.left();
    require(editor.cursor() == 1, "left steps back over a two-byte character");
    require(editor.backspace() && editor.text() == "éllo" && editor.cursor() == 0, "backspace");
    require(!editor.backspace(), "nothing before the start");
    require(editor.erase_forward() && editor.text() == "llo", "delete removes a whole character");
    editor.set_cursor(99);
    require(editor.cursor() == 3, "the cursor stays inside the text");
    editor.clear();
    editor.type("é");
    editor.set_cursor(1);
    require(editor.cursor() == 0, "a cursor inside a character moves to its start");
    editor.clear();
    require(editor.type("a\tb\x07\nc") && editor.text() == "ab\nc",
            "tabs and controls are dropped; line breaks stay");
    require(!editor.type("\t") && !editor.type("bad \xff"), "nothing usable changes nothing");
}

void limits() {
    DictationEditor editor;
    editor.type(std::string(899, 'a'));
    require(editor.can_dictate(), "dictation continues below 900");
    editor.type("é");
    require(!editor.can_dictate() && editor.length() == 900, "dictation stops at 900");
    editor.type(std::string(150, 'b'));
    require(editor.length() == DictationEditor::hard_limit, "typing stops at the hard limit");
    require(!editor.type("c") && !editor.insert_phrase("more", true),
            "nothing fits past the hard limit");
    editor.clear();
    editor.type(std::string(995, 'a'));
    require(editor.insert_phrase("abcdefgh", true) && editor.length() == 1000 &&
                editor.text().ends_with(" abcd"),
            "a phrase is cut at the hard limit, after its joining space");
}

void joining() {
    DictationEditor editor;
    require(editor.insert_phrase("  Hello there.  ", true) && editor.text() == "Hello there.",
            "a first phrase is trimmed and keeps its period at the end of the text");
    editor.insert_phrase("How are you?", true);
    require(editor.text() == "Hello there. How are you?", "a space joins it to the text before");
    editor.insert_phrase(", friend", true);
    require(editor.text() == "Hello there. How are you?, friend", "no space before closing punctuation");

    editor.clear();
    editor.type("Start end");
    editor.set_cursor(5);
    editor.insert_phrase("and middle.", true);
    require(editor.text() == "Start and middle end",
            "spaces on both sides and no period when text follows");
    require(editor.cursor() == std::string("Start and middle").size(),
            "the cursor ends after the phrase");

    editor.clear();
    editor.type("Start.");
    editor.set_cursor(5);
    editor.insert_phrase("Middle", true);
    require(editor.text() == "Start Middle.", "no space before following punctuation");

    editor.clear();
    editor.type("こんにちは。");
    editor.set_cursor(std::string("こんにちは").size());
    editor.insert_phrase("元気です。", false);
    require(editor.text() == "こんにちは元気です。",
            "no spaces in Japanese, and 。 dropped when text follows");
    require(!editor.insert_phrase("   ", true), "an empty phrase inserts nothing");
}

void undo() {
    DictationEditor editor;
    editor.insert_phrase("One.", true);
    editor.insert_phrase("Two.", true);
    require(editor.can_undo() && editor.undo_phrase() && editor.text() == "One." &&
                editor.cursor() == editor.text().size(),
            "undo removes the last phrase with its joining space");
    require(editor.undo_phrase() && editor.text().empty() && !editor.can_undo(),
            "repeated undo removes earlier ones");
    require(!editor.undo_phrase(), "nothing left to undo");

    editor.insert_phrase("Alpha.", true);
    editor.insert_phrase("Beta.", true);
    editor.set_cursor(0);
    editor.type("X ");
    require(editor.undo_phrase() && editor.text() == "X Alpha.",
            "typing before a phrase keeps it undoable");
    require(editor.cursor() == 2, "a cursor before the removed phrase stays");

    editor.clear();
    editor.insert_phrase("Gamma.", true);
    editor.insert_phrase("Delta.", true);
    editor.set_cursor(std::string("Gamma. Del").size());
    editor.backspace();
    require(editor.undo_phrase() && editor.text() == " Deta." && !editor.can_undo(),
            "an edited phrase is skipped; the earlier one goes");
    editor.clear();
    editor.insert_phrase("Edge", true);
    editor.type("!");
    require(editor.undo_phrase() && editor.text() == "!",
            "typing right after a phrase leaves it intact");
}

void sent_text() {
    DictationEditor editor;
    editor.insert_phrase("One.", true);
    editor.insert_phrase("Two.", true);
    editor.set_cursor(6);
    editor.erase_front(4);
    require(editor.text() == " Two." && editor.cursor() == 2,
            "delivered text leaves the front; the cursor stays on its character");
    require(editor.undo_phrase() && editor.text().empty(),
            "a phrase after the delivered text stays undoable");
    editor.type("abcdef");
    editor.erase_front(99);
    require(editor.empty() && editor.cursor() == 0, "erasing past the end empties the editor");

    editor.type("abcdef");
    editor.mark_uncertain(3);
    require(editor.uncertain() == 3, "the uncertain start is marked");
    editor.type("g");
    require(editor.uncertain() == 3, "typing after the mark keeps it");
    editor.set_cursor(3);
    editor.type("h");
    require(editor.uncertain() == 3, "typing right after the mark keeps it too");
    editor.set_cursor(2);
    editor.backspace();
    require(editor.uncertain() == 0, "correcting the marked text removes the mark");
    editor.mark_uncertain(99);
    require(editor.uncertain() == editor.text().size(), "the mark stays inside the text");
    editor.clear();
    require(editor.uncertain() == 0, "clearing removes it");
}

void context() {
    DictationEditor editor;
    std::string text;
    for (int i = 0; i < 250; ++i) {
        text += i < 50 ? "a" : "é";
    }
    editor.type(text);
    const auto context = editor.context();
    require(context == text.substr(50), "the prompt is the last 200 code points");
    editor.set_cursor(10);
    require(editor.context() == "aaaaaaaaaa", "only text before the cursor");
}
} // namespace

int main() {
    try {
        typing_and_cursor();
        limits();
        joining();
        undo();
        sent_text();
        context();
        std::cout << "Dictation editor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

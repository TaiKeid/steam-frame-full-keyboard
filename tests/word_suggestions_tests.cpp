// Typing suggestions' word lists, the user's learned words and completion
// (docs/typing-suggestions.md).
#include "framekeyboard/word_suggestions.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <sys/stat.h>

using namespace framekeyboard;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
WordList list(const std::string& text) {
    std::istringstream input(text);
    return WordList::parse(input);
}
using Words = std::vector<std::string>;
void list_tests() {
    const auto words = list("# comment\nhe\t220\nHe\t195\nhello\t200\nhelp\t190\nheld\t150\n"
                            "helmet\t100\nbad line\nempty\t\n\t5\nüber\t180\ndon't\t185\n");
    require(words.size() == 7, "comments, damaged lines and a second spelling of a key are left out");
    const auto he = words.find("he");
    require(he && he->spelling == "he" && he->frequency == 220, "a key keeps its commonest spelling");
    const auto found = words.starting("hel", 3);
    require(found.size() == 3 && found[0].key == "hello" && found[1].key == "help" &&
                found[2].key == "held",
            "words starting with a key come commonest first");
    require(words.starting("x", 3).empty() && !words.find("hel"), "nothing else matches");
    std::istringstream swiss("straße\t150\nstrasse\t20\n");
    const auto respelled = WordList::parse(swiss, {{"ß", "ss"}});
    require(respelled.size() == 1 && respelled.find("strasse")->frequency == 150,
            "a respelled word list writes ss for ß, its spellings merged");
}
void completion_tests() {
    const auto words = list("he\t220\nhello\t200\nhelp\t190\nheld\t150\nhelmet\t100\nüber\t180\n"
                            "don't\t185\niPhone\t120\n");
    const LearnedWords none;
    require(complete_word(words, none, "hel", 3) == Words{"hello", "help", "held"},
            "the commonest completions, as typed plus the rest");
    require(complete_word(words, none, "Hel", 3) == Words{"Hello", "Help", "Held"},
            "a capital typed stays");
    require(complete_word(words, none, "HEL", 3) == Words{"HELLO", "HELP", "HELD"},
            "two capitals or more make the rest capitals");
    require(complete_word(words, none, "he", 5) == Words{"hello", "help", "held", "helmet"},
            "the typed word itself is not offered");
    require(complete_word(words, none, "Üb", 3) == Words{"Über"} &&
                complete_word(words, none, "don", 3) == Words{"don't"},
            "letters beyond ASCII and apostrophes complete too");
    require(complete_word(words, none, "iph", 3) == Words{"iphone"}, "what was typed is never changed");
    require(complete_word(words, none, "", 3).empty() && complete_word(words, none, "xyz", 3).empty(),
            "nothing typed or nothing listed offers nothing");
    require(word_character('a') && word_character(0xfc) && word_character('\'') &&
                word_character(0x2019) && !word_character(' ') && !word_character('.') &&
                !word_character('1') && !word_character('-'),
            "words are letters, marks and apostrophes");
}
void learning_tests() {
    char directory[] = "/tmp/framekeyboard-words-XXXXXX";
    require(mkdtemp(directory), "temporary words folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    const auto file = fs::path(directory) / "words" / "en-us.tsv";
    const auto words = list("hello\t200\nhelp\t190\nheld\t150\nhelmet\t100\n");
    {
        LearnedWords learned(file);
        for (int i = 0; i < 10; ++i) {
            learned.record("helmet");
        }
        require(complete_word(words, learned, "hel", 3).front() == "helmet",
                "a word the user uses ranks higher");
        learned.record("hellraiser");
        learned.record("hellraiser");
        auto offered = complete_word(words, learned, "hell", 3);
        require(std::find(offered.begin(), offered.end(), "hellraiser") == offered.end(),
                "a new word waits for its third use");
        learned.record("Hellraiser");
        offered = complete_word(words, learned, "hell", 3);
        require(std::find(offered.begin(), offered.end(), "hellraiser") != offered.end(),
                "then it is offered, in the spelling last typed after what was typed");
        learned.record("a");
        require(learned.size() == 2, "one-letter words are not learned");
        require(learned.save(), "the words are saved");
        struct stat folder{}, saved{};
        require(stat(file.parent_path().c_str(), &folder) == 0 && (folder.st_mode & 077) == 0 &&
                    stat(file.c_str(), &saved) == 0 && (saved.st_mode & 077) == 0,
                "their folder and file are the owner's alone");
    }
    {
        LearnedWords again(file);
        require(again.uses("helmet") == 10 && again.uses("hellraiser") == 3, "and read back");
        again.clear();
        require(again.empty() && !fs::exists(file), "Clear forgets them and removes the file");
    }
    {
        LearnedWords many(file);
        for (std::size_t i = 0; i <= LearnedWords::most_words; ++i) {
            many.record("word" + std::to_string(i));
        }
        require(many.size() == LearnedWords::most_words && many.uses("word0") == 0 &&
                    many.uses("word" + std::to_string(LearnedWords::most_words)) == 1,
                "the least recent word goes past the limit");
    }
    {
        LearnedWords none;
        none.record("hello");
        require(none.empty(), "without a file nothing is kept");
    }
}
} // namespace
int main() {
    try {
        list_tests();
        completion_tests();
        learning_tests();
        std::cout << "Word lists, completion and learned words passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

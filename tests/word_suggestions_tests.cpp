// Typing suggestions' word lists, the user's learned words and completion
// (docs/configuration.md, Typing suggestions).
#include "framekeyboard/word_suggestions.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

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
    const auto elided = list("I'm\t160\nI'll\t150\nc'est\t170\n");
    require(complete_word(elided, none, "I'", 3) == Words{"I'm", "I'll"} &&
                complete_word(elided, none, "C'", 3) == Words{"C'est"},
            "a capital and an apostrophe aren't two capitals");
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
// An offensive word ranks below every other completion until the user has
// used it once, then as any other word.
void offensive_tests() {
    char directory[] = "/tmp/framekeyboard-words-XXXXXX";
    require(mkdtemp(directory), "temporary words folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::istringstream input("fuck\t155\nfucking\t154\nfun\t150\nfull\t140\nfuture\t130\nfully\t60\n");
    std::istringstream offensive("# held back\nFuck\nfucking\n");
    const auto words = WordList::parse(input, {}, &offensive);
    require(words.find("fuck")->offensive && words.find("fucking")->offensive &&
                !words.find("fun")->offensive,
            "the listed words are marked, whatever their case");
    const LearnedWords none;
    require(complete_word(words, none, "fu", 4) == Words{"fun", "full", "future", "fully"},
            "unused, they come after every other word");
    require(complete_word(words, none, "fuc", 3) == Words{"fuck", "fucking"},
            "and still complete what only they begin with");
    LearnedWords learned(fs::path(directory) / "en-us.tsv");
    learned.record("fuck");
    require(complete_word(words, learned, "fu", 3) == Words{"fuck", "fun", "full"},
            "once used, a word ranks as any other");
    const auto top = words.starting("f", 2);
    require(top.size() == 2 && top[0].key == "fun" && top[1].key == "full",
            "only the best few of many matches are kept, offensive ones last");
}
// Swiss German writes ss for ß and shares German's learned words.
void respelling_tests() {
    char directory[] = "/tmp/framekeyboard-words-XXXXXX";
    require(mkdtemp(directory), "temporary words folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::istringstream input("straße\t150\nstrom\t140\n");
    const auto swiss = WordList::parse(input, {{"ß", "ss"}});
    LearnedWords learned(fs::path(directory) / "de-de.tsv");
    for (int i = 0; i < 3; ++i) {
        learned.record("Fußweg");
    }
    const auto offered = complete_word(swiss, learned, "fu", 3);
    require(offered == Words{"fussweg"} || offered == Words{"Fussweg"},
            "a word learned with ß is offered as the list writes it");
}
// Turkish and Azerbaijani: I is the capital of ı and İ of i (R135).
void turkic_tests() {
    char directory[] = "/tmp/framekeyboard-words-XXXXXX";
    require(mkdtemp(directory), "temporary words folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    require(turkic_case("tr-TR") && turkic_case("az_AZ") && !turkic_case("en-US") &&
                !turkic_case("tt-RU"),
            "Turkish and Azerbaijani case I their own way");
    require(word_key("İyi", true) == "iyi" && word_key("IŞIK", true) == "ışık" &&
                word_key("Iyi") == "iyi",
            "Turkic keys lower İ to i and I to ı; others keep GLib's casing");
    std::istringstream input("iyi\t200\niçin\t190\nışık\t120\nılık\t100\nİngiliz\t80\n");
    std::istringstream offensive("ILIK\n"); // A stand-in, matched by its Turkic key.
    const auto turkish = WordList::parse(input, {}, &offensive, true);
    const LearnedWords none;
    require(turkish.find("ılık")->offensive, "offensive words are matched by Turkic keys");
    require(complete_word(turkish, none, "İ", 2) == Words{"İyi", "İçin"},
            "İ completes words spelled with i");
    require(complete_word(turkish, none, "I", 2) == Words{"Işık", "Ilık"},
            "I completes words spelled with ı, never i");
    require(complete_word(turkish, none, "İY", 1) == Words{"İYİ"}, "capitals keep the dotted İ");
    LearnedWords learned(fs::path(directory) / "tr-tr.tsv", true);
    learned.record("İyi");
    learned.record("iyi");
    learned.record("Işık");
    require(learned.size() == 2 && learned.uses("iyi") == 2 && learned.uses("ışık") == 1 &&
                learned.uses("işık") == 0,
            "a word is learned once whatever its first capital");
}
// The loader hands over the latest request only, and ends with its owner.
void loader_tests() {
    char directory[] = "/tmp/framekeyboard-words-XXXXXX";
    require(mkdtemp(directory), "temporary words folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    const fs::path folder = directory;
    std::ofstream(folder / "a.tsv") << "alpha\t100\n";
    std::ofstream(folder / "b.tsv") << "beta\t100\n";
    std::ofstream(folder / "b-offensive.tsv") << "beta\n";
    std::ofstream(folder / "learned.tsv") << "betamax\t4\t1\n";
    WordListLoader loader;
    require(loader.request({"a", folder / "a.tsv", {}, {}, {}}) &&
                loader.request(
                    {"b", folder / "b.tsv", folder / "b-offensive.tsv", folder / "learned.tsv", {}}),
            "requests are taken");
    std::optional<WordListLoader::Result> result;
    for (int i = 0; i < 2000 && !(result = loader.take()); ++i) {
        usleep(1000);
    }
    require(result && result->id == "b" && result->list->find("beta")->offensive &&
                result->learned.loaded() && result->learned.uses("betamax") == 4,
            "the latest request is loaded, with its offensive words and the user's words");
    require(!loader.take(), "once");
    loader.request({"a", folder / "a.tsv", {}, {}, {}}); // Still loading as the loader ends.
}
} // namespace
int main() {
    try {
        list_tests();
        completion_tests();
        learning_tests();
        offensive_tests();
        respelling_tests();
        turkic_tests();
        loader_tests();
        std::cout << "Word lists, completion and learned words passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

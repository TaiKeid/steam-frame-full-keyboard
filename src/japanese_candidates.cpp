#include "framekeyboard/japanese_candidates.hpp"
#include "framekeyboard/emoji_words.hpp"
#include "framekeyboard/swipe.hpp"
#include <glib.h>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>

namespace framekeyboard {
namespace {
// The first `count` texts, separated so that a candidate with spaces reads whole.
std::string listed(const std::vector<std::string>& texts, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < texts.size() && i < count; ++i) {
        result += (i ? " | " : "") + texts[i];
    }
    if (texts.size() > count) {
        result += " | … " + std::to_string(texts.size() - count) + " more";
    }
    return result;
}
// Types a phrase as the keyboard would: romaji letter by letter, kana at once.
void type(JapaneseComposer& composer, const std::string& phrase) {
    bool romaji = false;
    for (const char c : phrase) {
        romaji |= g_ascii_isalpha(c);
    }
    if (!romaji) {
        std::string kana;
        for (const char c : phrase) {
            if (c != ' ') {
                kana += c;
            }
        }
        composer.kana(kana);
        return;
    }
    for (const char* p = phrase.c_str(); *p; p = g_utf8_next_char(p)) {
        const auto size = static_cast<std::size_t>(g_utf8_next_char(p) - p);
        if (size == 1 && *p != ' ') {
            composer.roman(*p);
        } else if (size > 1) {
            composer.kana(std::string(p, size));
        }
    }
}
} // namespace
void report_japanese_candidates(std::istream& phrases, std::ostream& report,
                                const JapaneseCandidatesOptions& options) {
    // The keyboard's own lists and emoji (App's constructor), without learning.
    JapaneseWords words(data_resource(options.data_dir, "japanese/words.tsv"));
    JapaneseWords kaomoji(data_resource(options.data_dir, "japanese/kaomoji.tsv"));
    JapaneseWords preferred(data_resource(options.data_dir, "japanese/preferred.tsv"));
    JapaneseComposer composer(options.converter);
    composer.set_emoji([](const std::vector<std::string>& found) { return emoji_for(found, 4); });
    composer.set_words(&words);
    composer.set_kaomoji(&kaomoji);
    composer.set_phrases(&preferred);
    composer.set_preferred([&](const std::string& reading) { return preferred.exact(reading, 1); });
    std::string phrase;
    while (std::getline(phrases, phrase)) {
        if (phrase.empty() || phrase.front() == '#') {
            report << phrase << '\n';
            continue;
        }
        composer.cancel();
        type(composer, phrase);
        std::string offers;
        try {
            composer.prepare_live();
            offers = "  live  " + listed(composer.live(), options.live) + "\n  変換 ";
            composer.convert();
            for (int i = 0; i < composer.segment_count(); ++i) {
                offers += " [" + listed(composer.candidates(), options.segment) + ']';
                composer.segment(1);
            }
        } catch (const std::exception& error) {
            offers = std::string("  error ") + error.what();
        }
        // The reading converted, a pending n as ん.
        report << phrase;
        if (composer.reading() != phrase) {
            report << " → " << composer.reading();
        }
        report << '\n' << offers << '\n';
    }
    composer.cancel();
}
} // namespace framekeyboard

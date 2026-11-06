#include "framekeyboard/japanese_candidates.hpp"
#include "framekeyboard/emoji_words.hpp"
#include "framekeyboard/japanese_worker.hpp"
#include "framekeyboard/mozc.hpp"
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
// A step of the composer's as the keyboard takes it: here if it converts
// nothing, else on the worker, from the composer as it was, waiting for the
// answer as only the report does. An error comes back as the step's own.
void run_step(JapaneseWorker& worker, JapaneseComposer& composer, JapaneseWorker::Kind kind,
              const JapaneseWorker::Step& step) {
    auto before = composer;
    try {
        step(composer);
        return;
    } catch (const JapaneseComposer::Deferred&) {
        composer.adopt(std::move(before));
    }
    worker.post(kind, composer, step);
    worker.wait();
    for (auto& answer : worker.take()) {
        if (!answer.composer) {
            // Learning's answer carries nothing back.
        } else if (kind == JapaneseWorker::Kind::Live) {
            composer.adopt_live(std::move(*answer.composer));
        } else {
            composer.adopt(std::move(*answer.composer));
        }
        if (!answer.error.empty()) {
            throw std::runtime_error(answer.error);
        }
    }
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
    auto converter = options.converter;
    if (!options.mozc_server.empty()) {
        start_mozc(options.mozc_server, options.mozc_profile,
                   data_resource(options.data_dir, "japanese/mozc-words.tsv"));
        if (!wait_for_mozc(std::chrono::seconds(10))) {
            throw std::runtime_error("Mozc did not start");
        }
        // As in the simulator, Mozc is waited for rather than left to Anthy.
        converter = [] { return mozc_converter(anthy_converter, std::chrono::seconds(3)); };
    }
    // The worker converts, as on the keyboard (docs/architecture.md,
    // Japanese conversion), each step waited for.
    JapaneseWorker worker(converter);
    JapaneseComposer composer;
    composer.defer_conversion();
    composer.set_word_forms(options.word_forms);
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
        // Which engine converted each: Mozc, or Anthy without Mozc or while
        // it couldn't answer.
        const auto engine = [&] {
            return !options.mozc_server.empty() && !composer.fell_back() ? "(Mozc)" : "(Anthy)";
        };
        std::string offers;
        try {
            // Typing too may refuse a phrase, as one past the reading's
            // length limit: it gets an error line, and the next one goes on.
            type(composer, phrase);
            run_step(worker, composer, JapaneseWorker::Kind::Live, [](JapaneseComposer& copy) {
                copy.prepare_live();
                return true;
            });
            offers = std::string("  live ") + engine() + "  " + listed(composer.live(), options.live);
            run_step(worker, composer, JapaneseWorker::Kind::Step, [](JapaneseComposer& copy) {
                copy.convert();
                return true;
            });
            offers += std::string("\n  変換 ") + engine() + " ";
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

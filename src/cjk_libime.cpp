// Pinyin with libime: sentence-level conversion with an n-gram model, as in
// Fcitx 5. Traditional Pinyin converts libime's Simplified text with OpenCC's
// Taiwan phrases (s2twp).
#include "cjk_engine.hpp"
#include "cjk_module.hpp"
#include "framekeyboard/private_file.hpp"
#include "opencc_context.hpp"
#include "pinyin_reranker.hpp"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindecoder.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>
#include <opencc/Config.hpp>
#include <opencc/Exception.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace framekeyboard {
namespace {
namespace fs = std::filesystem;
// A longer reading sends its converted front first, as a PC input method
// does, and goes on.
constexpr std::size_t max_keys = 30;
// Bounds candidate work and the menu for controller interaction.
constexpr std::size_t max_candidates = 50;
// Sentences the Zhuyin sentence experiment weighs (CjkEngine::prefer()).
constexpr std::size_t proposed_sentences = 20;
constexpr const char* sentences_method = "chinese-pinyin-traditional-sentences";

std::optional<PinyinReranker> experimental_reranker(const std::string& method) {
    const auto* directory = std::getenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY");
    if (!directory || !*directory ||
        (method != "chinese-pinyin-simplified" && method != "chinese-pinyin-traditional")) {
        return std::nullopt;
    }
    // Both scripts write the same learning files. A half-valid bundle must
    // not split their mutable Libraries between n-best=2 and n-best=20.
    auto simplified = PinyinReranker::load(fs::path(directory) / "simplified.fkpr");
    auto traditional = PinyinReranker::load(fs::path(directory) / "traditional.fkpr");
    if (!simplified || !traditional) {
        return std::nullopt;
    }
    return method == "chinese-pinyin-simplified" ? std::move(simplified) : std::move(traditional);
}

bool experimental_search() {
    const auto* value = std::getenv("FRAMEKEYBOARD_PINYIN_NBEST");
    return value && std::string_view(value) == "20";
}

// Guards the Library cache, the model cache and each Library's converter
// slot, which framekeyboard_prepare_cjk_v1 fills outside the engines' lock.
// Everything else in a Library is only used under the engines' lock.
std::mutex& library_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Dictionaries and learned history belong to each Library. The immutable
// model does not: Pinyin and Zhuyin's proposer can share it even with different
// learning folders and n-best counts. The last context still releases it.
std::shared_ptr<const libime::StaticLanguageModelFile> static_model(const fs::path& data) {
    static std::weak_ptr<const libime::StaticLanguageModelFile> cached;
    auto model = cached.lock();
    if (!model) {
        model = std::make_shared<libime::StaticLanguageModelFile>((data / "zh_CN.lm").c_str());
        cached = model;
    }
    return model;
}

// The dictionaries, model and what was learned, loaded once for each learning
// folder and shared by its contexts: a profile change briefly has two, and an
// added Chinese language keeps one waiting while another language is in use.
struct Library {
    fs::path learning; // Empty, or after a learned file failed to load: nothing is saved.
    // Learned since the last save: written on an idle tick
    // (framekeyboard_save_cjk_v1), or when the last engine using it goes.
    bool unsaved = false;
    std::unique_ptr<libime::PinyinIME> ime;
    // OpenCC's Taiwan converter (~50 MB), shared while a Traditional engine
    // holds it: Simplified engines keep the Library but not the converter,
    // so it goes with the last Traditional engine (R141).
    std::weak_ptr<opencc::Converter> traditional;

    Library(const fs::path& dir, std::size_t sentences) : learning(dir) {
        const auto data = engine_data_dir() / "libime";
        auto dict = std::make_unique<libime::PinyinDictionary>();
        std::unique_ptr<libime::UserLanguageModel> model;
        try {
            dict->load(libime::PinyinDictionary::SystemDict, (data / "sc.dict").c_str(),
                       libime::PinyinDictFormat::Binary);
            // Rare characters (CJK Extension B), after the user's words, as in Fcitx 5.
            dict->addEmptyDict();
            dict->load(libime::PinyinDictionary::UserDict + 1, (data / "extb.dict").c_str(),
                       libime::PinyinDictFormat::Binary);
            model = std::make_unique<libime::UserLanguageModel>(static_model(data));
        } catch (const std::exception&) {
            throw CjkMessage("ime.pinyin-dictionary-unavailable");
        }
        // A learned file that can't be read is left as it is, never overwritten.
        if (!learning.empty()) {
            try {
                if (std::ifstream in(learning / "user.dict", std::ios::binary); in) {
                    dict->load(libime::PinyinDictionary::UserDict, in, libime::PinyinDictFormat::Binary);
                }
                if (std::ifstream in(learning / "user.history", std::ios::binary); in) {
                    model->load(in);
                }
            } catch (const std::exception&) {
                learning.clear();
            }
        }
        ime = std::make_unique<libime::PinyinIME>(std::move(dict), std::move(model));
        // Fcitx 5's defaults: common typos, ue for ve, and partial syllables.
        libime::PinyinFuzzyFlags fuzzy{libime::PinyinFuzzyFlag::CommonTypo};
        fuzzy |= libime::PinyinFuzzyFlag::VE_UE;
        fuzzy |= libime::PinyinFuzzyFlag::Inner;
        fuzzy |= libime::PinyinFuzzyFlag::InnerShort;
        fuzzy |= libime::PinyinFuzzyFlag::PartialFinal;
        ime->setFuzzyFlags(fuzzy);
        ime->setNBest(sentences);
    }
    opencc::ConverterPtr open_traditional() {
        std::lock_guard lock(library_mutex());
        auto converter = traditional.lock();
        if (!converter) {
            try {
                converter =
                    opencc::Config().NewFromFile((engine_data_dir() / "opencc/s2twp.json").string());
            } catch (...) { // OpenCC's own, or its dictionaries' (a corrupt .ocd2).
                throw CjkMessage("ime.pinyin-dictionary-unavailable");
            }
            traditional = converter;
        }
        return converter;
    }
    static std::string to_traditional(const opencc::ConverterPtr& converter, const std::string& text,
                                      const std::string& prefix, const std::string& selected) {
        try {
            if (!prefix.empty()) {
                if (auto suffix = contextual_suffix(converter, prefix, text, selected)) {
                    return *suffix;
                }
            }
            return converter->Convert(text);
        } catch (const opencc::Exception&) {
            return text;
        }
    }
    ~Library() { save_unsaved(); }
    void save_unsaved() noexcept {
        if (!unsaved) {
            return;
        }
        unsaved = false;
        try {
            save();
        } catch (...) {
            // Kept in memory; the next learned commit tries again.
        }
    }
    // Small files, written whole: never on the commit itself (R142).
    void save() const {
        if (learning.empty() || !make_learning_dir(learning)) {
            return;
        }
        std::ostringstream history, user;
        ime->model()->save(history);
        ime->dict()->save(libime::PinyinDictionary::UserDict, user, libime::PinyinDictFormat::Binary);
        write_private_file(learning / "user.history", history.str());
        write_private_file(learning / "user.dict", user.str());
    }
};
// By learning folder and the number of whole sentences offered.
std::map<std::pair<fs::path, std::size_t>, std::weak_ptr<Library>>& library_cache() {
    static std::map<std::pair<fs::path, std::size_t>, std::weak_ptr<Library>> libraries;
    return libraries;
}
std::shared_ptr<Library> library_for(const fs::path& learning, std::size_t sentences) {
    std::lock_guard lock(library_mutex());
    auto& slot = library_cache()[{learning, sentences}];
    auto library = slot.lock();
    if (!library) {
        library = std::make_shared<Library>(learning, sentences);
        slot = library;
    }
    return library;
}
// A method's Library: Zhuyin's sentence proposer learns nothing, and it and
// the wider search ask for many sentences.
std::shared_ptr<Library> library_for(bool sentences, bool wider_search, const fs::path& learning_dir) {
    return library_for(learning_dir.empty() || sentences ? fs::path{} : learning_dir / "libime",
                       sentences || wider_search ? proposed_sentences : 2);
}
void clear_libraries(const fs::path& learning) {
    std::lock_guard lock(library_mutex());
    for (const auto& [key, weak] : library_cache()) {
        if (const auto library = weak.lock(); library && key.first == learning) {
            library->ime->model()->history().clear();
            library->ime->dict()->clear(libime::PinyinDictionary::UserDict);
            library->unsaved = false;
            library->learning = learning; // A file it couldn't read goes too: it learns again.
        }
    }
}
void save_libraries() {
    std::lock_guard lock(library_mutex());
    for (const auto& [key, weak] : library_cache()) {
        if (const auto library = weak.lock()) {
            library->save_unsaved();
        }
    }
}
// Kept from framekeyboard_prepare_cjk_v1 until create() holds its own.
struct PreparedLibrary {
    std::shared_ptr<Library> library;
    opencc::ConverterPtr converter;
};

class LibimeEngine final : public CjkEngine {
  public:
    // The sentences method proposes Traditional sentences for Zhuyin: many,
    // learning nothing, and with no limit on the reading.
    LibimeEngine(const std::string& method, const fs::path& learning_dir)
        : sentences_(method == sentences_method), reranker_(experimental_reranker(method)),
          wider_search_(!sentences_ && (reranker_.has_value() || experimental_search())),
          library_(library_for(sentences_, wider_search_, learning_dir)), context_(library_->ime.get()),
          traditional_(method == "chinese-pinyin-traditional" || sentences_) {
        if (method != "chinese-pinyin-simplified" && !traditional_) {
            throw CjkMessage("ime.engine-missing");
        }
        if (traditional_) {
            converter_ = library_->open_traditional();
        }
    }
    void type(char key) override {
        if (context_.empty() && front_.empty()) {
            front_produced_ = false; // A new reading after an emptied/cancelled composition.
        }
        if (context_.size() >= max_keys && !sentences_) {
            send_front();
        }
        context_.type(
            std::string(1, static_cast<char>(key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key)));
        refresh();
    }
    void backspace() override {
        if (context_.size() > context_.selectedLength()) {
            context_.backspace();
        } else if (!chosen_.empty()) {
            // Nothing left to delete: the last choice is undone instead.
            context_.cancelTill(chosen_.back());
            chosen_.pop_back();
            chosen_display_.pop_back();
            defaulted_.pop_back();
            picked_.resize(std::min(picked_.size(), context_.selectedWords().size()));
        } else if (!front_.empty()) {
            front_.resize(utf8_prefix(front_, utf8_length(front_) - 1));
        }
        refresh();
    }
    void cancel() override {
        context_.clear();
        chosen_.clear();
        chosen_display_.clear();
        defaulted_.clear();
        picked_.clear();
        front_.clear();
        front_produced_ = false;
        refresh();
    }
    // Esc: the reading goes, and front text the field refused stays to go
    // with the next commit (R149); with only that left, it goes too (R156).
    void cancel_open() override {
        if (context_.empty()) {
            cancel();
            return;
        }
        auto front = std::move(front_);
        cancel();
        front_ = std::move(front);
        front_produced_ = !front_.empty();
        refresh();
    }
    void cycle(int direction) override {
        if (const int count = static_cast<int>(shown_.size())) {
            selected_ = (selected_ + direction % count + count) % count;
        }
    }
    void choose(int index) override {
        if (index < 0 || index >= static_cast<int>(shown_.size())) {
            return;
        }
        const auto before = context_.selectedWords().size();
        // After an earlier choice, the top candidate for the rest is what
        // the user accepted, not picked: 来, 一局, then Space for 马.
        defaulted_.push_back(index == 0 && !chosen_.empty());
        chosen_.push_back(context_.selectedLength());
        chosen_display_.push_back(shown_[static_cast<std::size_t>(index)]);
        context_.select(source_[static_cast<std::size_t>(index)]);
        const auto words = context_.selectedWords();
        picked_.resize(before);
        for (std::size_t i = before; i < words.size(); ++i) {
            picked_.push_back(index != 0);
        }
        refresh();
    }
    // The syllables shown apart, as libime splits them: ni hao. Without a
    // conversion libime shows none of them, so they show as typed.
    std::string preedit() const override {
        const auto chosen = context_.selectedSentence();
        auto rest = context_.preedit(libime::PinyinPreeditMode::RawText).substr(chosen.size());
        if (rest.empty()) {
            rest = context_.userInput().substr(context_.selectedLength());
        }
        return front_ + selected_display() + rest;
    }
    std::string text() const override {
        if (typed_) {
            return front_ + selected_display() + context_.userInput().substr(context_.selectedLength());
        }
        return front_ + default_text();
    }
    std::string finished() const override { return front_; }
    void drop_finished() override { front_.clear(); }
    void as_typed() override { typed_ = true; }
    void type_reading(const std::string& keys) override {
        cancel();
        context_.type(keys);
        refresh();
    }
    std::vector<std::string> candidates() const override { return shown_; }
    int selected() const override { return selected_; }
    bool empty() const override { return context_.empty() && front_.empty(); }
    bool has_reading() const override { return context_.size() > context_.selectedLength(); }
    bool chinese() const override { return true; }
    void committed(bool learn) override {
        if (!learn || library_->learning.empty() || (typed_ && !context_.selected())) {
            return; // Letters typed as they are teach nothing.
        }
        // Without Enter's letters, the top candidate for what was left went
        // in; it counts as accepted.
        for (std::size_t i = 0; i < max_keys && !context_.selected() && !context_.candidates().empty();
             ++i) {
            // The delivered default may differ from libime's original row 0.
            // Select exactly that source before learning its words/history.
            defaulted_.push_back(!chosen_.empty());
            chosen_.push_back(context_.selectedLength());
            chosen_display_.push_back(shown_.front());
            context_.select(default_source());
            refresh();
        }
        if (!context_.selected()) {
            return;
        }
        if (std::find(defaulted_.begin(), defaulted_.end(), true) == defaulted_.end()) {
            context_.learn(); // The whole sentence, as a word of its own if picked in parts.
        } else {
            // Learn each pick alone, with no sentence transitions: even a
            // contiguous 来, 一局 run can promote an accepted 马 tail.
            // With its Pinyin, so a pick raises the character only for the
            // reading it was picked with (R139); a word without Pinyin is
            // left out, as libime's own learn() does.
            if (!remember_picked_words(context_.selectedWordsWithPinyin(), picked_,
                                       [&](const auto& word) {
                                           if (!word.front().second.empty()) {
                                               library_->ime->model()->history().addWithCode(word);
                                           }
                                       })) {
                return;
            }
        }
        library_->unsaved = true;
    }

  private:
    bool sentences_; // Before library_, which it chooses.
    std::optional<PinyinReranker> reranker_;
    bool wider_search_;
    std::shared_ptr<Library> library_; // Before context_, which uses its IME.
    libime::PinyinContext context_;
    bool traditional_;
    opencc::ConverterPtr converter_; // Traditional only: keeps the shared converter loaded.
    int selected_{};
    std::vector<std::size_t> chosen_; // selectedLength() before each choice.
    // The displayed script belongs to the user's selection. Keep it apart
    // from libime's Simplified words/readings, which still own learning.
    std::vector<std::string> chosen_display_; // Aligned with chosen_ for undo.
    std::vector<bool> picked_;                // Each selected word: picked, not the top candidate.
    // Aligned with chosen_: a top candidate taken after an earlier choice.
    // Undoing that choice undoes its flag (R147).
    std::vector<bool> defaulted_;
    bool typed_{};          // Enter: the rest goes as typed (as_typed()).
    bool front_produced_{}; // Survives drop_finished() until this reading ends.
    std::string front_;     // Sent ahead of a long reading (finished()).
    // Candidates as shown, Traditional ones once each, and libime's index of each.
    std::vector<std::string> shown_;
    std::vector<std::size_t> source_;

    std::size_t default_source() const { return source_.empty() ? 0 : source_.front(); }
    std::string selected_display() const {
        std::string result;
        for (const auto& text : chosen_display_) {
            result += text;
        }
        return result;
    }
    std::string default_text() const {
        const auto& candidates = context_.candidates();
        // OpenCC may choose a contextual spelling for the unselected suffix.
        // It must never reinterpret an already displayed and selected prefix.
        return selected_display() +
               (candidates.empty() ? std::string{} : shown(candidates[default_source()].toString()));
    }
    bool complete(const libime::SentenceResult& candidate) const {
        const auto& sentence = candidate.sentence();
        // Lattice endpoints are relative to the unselected suffix.
        return !sentence.empty() &&
               sentence.back()->to()->index() + context_.selectedLength() == context_.size();
    }

    bool learned_default(const libime::SentenceResult& candidate) const {
        if (library_->learning.empty()) {
            return false;
        }
        std::string code;
        for (const auto* node : candidate.sentence()) {
            const auto& part = node->as<libime::PinyinLatticeNode>().encodedPinyin();
            if (part.empty() || part.size() % 2 != 0) {
                return false;
            }
            code += part;
        }
        const auto& history = library_->ime->model()->history();
        if (history.rawUnigramFrequency({candidate.toString(), code}) > 0) {
            return true;
        }
        // Native history may store a correction as several words. Require
        // its entire coded path, including both native sentence boundaries;
        // familiar components alone are insufficient. This honors n-gram
        // preference, not proof that a recombined sentence was seen verbatim.
        using Word = libime::HistoryBigram::WordWithCodeView;
        Word previous{"<s>", ""};
        for (const auto* node : candidate.sentence()) {
            const Word current{node->word(), node->as<libime::PinyinLatticeNode>().encodedPinyin()};
            if (history.rawBigramFrequency(previous, current) <= 0) {
                return false;
            }
            previous = current;
        }
        return history.rawBigramFrequency(previous, {"</s>", ""}) > 0;
    }
    std::string shown(const std::string& text) const {
        return traditional_ && !text.empty()
                   ? Library::to_traditional(converter_, text, context_.selectedSentence(),
                                             selected_display())
                   : text;
    }
    void refresh() {
        shown_.clear();
        source_.clear();
        selected_ = 0;
        typed_ = false;
        const auto& candidates = context_.candidates();
        std::vector<std::pair<std::string, std::size_t>> rows;
        // Reserve room for the extra search proposals without displacing the
        // normal fifty correction choices from the expanded menu.
        const auto menu_limit = max_candidates + (wider_search_ ? proposed_sentences : 0);
        // Search may consider twenty sentences. Keep the winner first, then
        // ordinary words before the other multiword sentence hypotheses.
        // Full-input single words and short-reading homophones remain choices.
        std::size_t winner = 0;
        // Retain isolated-character frequency and a native winner positively
        // learned as a whole reading. Do not infer a whole-reading preference
        // from selected prefixes, a long reading's tail or familiar components.
        const bool native_preference =
            reranker_ && context_.selectedLength() == 0 && !front_produced_ && !candidates.empty() &&
            complete(candidates.front()) &&
            (utf8_length(candidates.front().toString()) == 1 || learned_default(candidates.front()));
        if (reranker_ && !candidates.empty() && !native_preference) {
            double best = -std::numeric_limits<double>::infinity();
            double base = 0;
            std::size_t rank = 0;
            std::vector<std::string> seen;
            for (std::size_t i = 0; i < candidates.size() && rank < max_candidates; ++i) {
                if (!complete(candidates[i])) {
                    continue;
                }
                if (!rank) {
                    base = candidates[i].score();
                }
                const auto text = shown(candidates[i].toString());
                const auto current_rank = rank++;
                if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
                    continue;
                }
                seen.push_back(text);
                std::vector<std::string_view> words;
                for (const auto* node : candidates[i].sentence()) {
                    words.push_back(node->word());
                }
                const auto score =
                    reranker_->score(text, words, current_rank, candidates[i].score(), base);
                if (score > best) {
                    best = score;
                    winner = i;
                }
            }
        }
        if (wider_search_ && !candidates.empty()) {
            rows.emplace_back(shown(candidates[winner].toString()), winner);
        }
        for (std::size_t i = 0; i < candidates.size() && rows.size() < menu_limit; ++i) {
            // Alternative syllable splits can consume the same reading with
            // different numbers of characters. Only the input endpoint tells
            // whether this is a whole sentence rather than a prefix word.
            const bool full = complete(candidates[i]);
            if (sentences_ && !full) {
                continue;
            }
            auto text = shown(candidates[i].toString());
            if (std::none_of(rows.begin(), rows.end(),
                             [&](const auto& row) { return row.first == text; })) {
                rows.emplace_back(std::move(text), i);
            }
        }
        if (wider_search_ && rows.size() > 1) {
            std::stable_partition(rows.begin() + 1, rows.end(), [&](const auto& row) {
                const auto& candidate = candidates[row.second];
                return !complete(candidate) || candidate.sentence().size() == 1;
            });
        }
        // A rare single character waits behind the rest: neng offers 能 and
        // its common homophones before 㲌. The conversion stays first.
        if (!sentences_ && rows.size() > 1) {
            std::stable_partition(rows.begin() + 1, rows.end(), [](const auto& row) {
                return utf8_length(row.first) > 1 || !rare_character(row.first);
            });
        }
        for (auto& [text, index] : rows) {
            shown_.push_back(std::move(text));
            source_.push_back(index);
        }
    }
    // The reading is full: the best conversion's first word goes ahead, to
    // the field (finished()), and the rest is typed again behind it.
    void send_front() {
        const auto& candidates = context_.candidates();
        if (candidates.empty() || candidates[default_source()].sentence().empty()) {
            throw CjkMessage("ime.commit-first"); // Nothing converts: it must be corrected.
        }
        const auto* first = candidates[default_source()].sentence().front();
        auto display = selected_display();
        auto first_display = shown(first->word());
        // The row showed OpenCC's conversion of the whole sentence, which a
        // phrase across the first word's end can change: send that row's
        // own start where the conversion kept the length (R151).
        const auto sentence = candidates[default_source()].toString();
        if (const auto whole = shown(sentence);
            !whole.starts_with(first_display) && utf8_length(whole) == utf8_length(sentence)) {
            first_display = whole.substr(0, utf8_prefix(whole, utf8_length(first->word())));
        }
        const auto before = context_.selectedLength();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& sentence = candidates[i].sentence();
            if (sentence.size() == 1 && sentence.front()->word() == first->word() &&
                sentence.front()->to()->index() == first->to()->index()) {
                context_.select(i);
                break;
            }
        }
        if (!context_.selectedLength()) {
            throw CjkMessage("ime.commit-first");
        }
        // A corrected/fuzzy first word need not have a separate prefix row.
        // In that case only the already selected prefix can be sent; adding
        // the unselected first word here would duplicate it in the remainder.
        if (context_.selectedLength() > before) {
            display += first_display;
        }
        front_ += display;
        front_produced_ = true;
        const auto rest = context_.userInput().substr(context_.selectedLength());
        context_.clear();
        chosen_.clear();
        chosen_display_.clear();
        defaulted_.clear();
        picked_.clear();
        context_.type(rest);
    }
};
} // namespace
} // namespace framekeyboard

// The only exported entry point. The module and host ship in the same release
// and share this internal C++ interface, not libime's ABI.
extern "C" __attribute__((visibility("default"))) framekeyboard::CjkEngine*
framekeyboard_create_cjk_v3(const char* method, const char* learning_dir) {
    return new framekeyboard::LibimeEngine(method, learning_dir ? learning_dir : "");
}
// The default Pinyin engines' Library and converter, which create() then finds
// loaded. The opt-in reranker and Zhuyin's proposer load in create().
extern "C" __attribute__((visibility("default"))) void*
framekeyboard_prepare_cjk_v1(const char* method, const char* learning_dir) {
    using namespace framekeyboard;
    const std::string name = method ? method : "";
    const bool traditional = name == "chinese-pinyin-traditional";
    const char* reranker = std::getenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY");
    if ((!traditional && name != "chinese-pinyin-simplified") || (reranker && *reranker)) {
        return nullptr;
    }
    try {
        auto library = library_for(false, experimental_search(), learning_dir ? learning_dir : "");
        auto converter = traditional ? library->open_traditional() : nullptr;
        return new PreparedLibrary{std::move(library), std::move(converter)};
    } catch (...) {
        return nullptr; // create() reports it.
    }
}
extern "C" __attribute__((visibility("default"))) void
framekeyboard_release_prepared_cjk_v1(void* prepared) {
    delete static_cast<framekeyboard::PreparedLibrary*>(prepared);
}
extern "C" __attribute__((visibility("default"))) void framekeyboard_save_cjk_v1() {
    framekeyboard::save_libraries();
}
extern "C" __attribute__((visibility("default"))) void
framekeyboard_clear_cjk_v1(const char* learning_dir) {
    if (learning_dir && *learning_dir) {
        framekeyboard::clear_libraries(std::filesystem::path(learning_dir) / "libime");
    }
}

// Pinyin with libime: sentence-level conversion with an n-gram model, as in
// Fcitx 5. Traditional Pinyin converts libime's Simplified text with OpenCC's
// Taiwan phrases (s2twp).
#include "cjk_engine.hpp"
#include "cjk_module.hpp"
#include "framekeyboard/private_file.hpp"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>
#include <opencc/opencc.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace framekeyboard {
namespace {
namespace fs = std::filesystem;
// A longer reading sends its converted front first, as a PC input method
// does, and goes on.
constexpr std::size_t max_keys = 30;
// Bounds candidate work and the menu for controller interaction.
constexpr std::size_t max_candidates = 50;
const auto opencc_failed = reinterpret_cast<opencc_t>(-1);

// The dictionaries, model and what was learned, loaded once for each learning
// folder and shared by its contexts: a profile change briefly has two, and an
// added Chinese language keeps one waiting while another language is in use.
struct Library {
    fs::path learning; // Empty, or after a learned file failed to load: nothing is saved.
    std::unique_ptr<libime::PinyinIME> ime;
    opencc_t traditional = opencc_failed;

    explicit Library(const fs::path& dir) : learning(dir) {
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
            model = std::make_unique<libime::UserLanguageModel>((data / "zh_CN.lm").c_str());
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
        ime->setNBest(2);
    }
    ~Library() {
        if (traditional != opencc_failed) {
            opencc_close(traditional);
        }
    }
    void open_traditional() {
        if (traditional == opencc_failed) {
            traditional = opencc_open((engine_data_dir() / "opencc/s2twp.json").c_str());
            if (traditional == opencc_failed) {
                throw CjkMessage("ime.pinyin-dictionary-unavailable");
            }
        }
    }
    std::string to_traditional(const std::string& text) const {
        char* converted = opencc_convert_utf8(traditional, text.data(), text.size());
        if (!converted) {
            return text;
        }
        std::string result = converted;
        opencc_convert_utf8_free(converted);
        return result;
    }
    // Small files, written whole after each learned commit.
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
std::shared_ptr<Library> library_for(const fs::path& learning) {
    static std::map<fs::path, std::weak_ptr<Library>> libraries;
    auto library = libraries[learning].lock();
    if (!library) {
        library = std::make_shared<Library>(learning);
        libraries[learning] = library;
    }
    return library;
}

class LibimeEngine final : public CjkEngine {
  public:
    LibimeEngine(const std::string& method, const fs::path& learning_dir)
        : library_(library_for(learning_dir.empty() ? fs::path{} : learning_dir / "libime")),
          context_(library_->ime.get()), traditional_(method == "chinese-pinyin-traditional") {
        if (method != "chinese-pinyin-simplified" && !traditional_) {
            throw CjkMessage("ime.engine-missing");
        }
        if (traditional_) {
            library_->open_traditional();
        }
    }
    void type(char key) override {
        if (context_.size() >= max_keys) {
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
            picked_.resize(std::min(picked_.size(), context_.selectedWords().size()));
        }
        refresh();
    }
    void cancel() override {
        context_.clear();
        chosen_.clear();
        picked_.clear();
        defaults_ = false;
        front_.clear();
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
        defaults_ |= index == 0 && !chosen_.empty();
        chosen_.push_back(context_.selectedLength());
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
        return front_ + shown(chosen) + rest;
    }
    std::string text() const override {
        if (typed_) {
            return front_ + shown(context_.selectedSentence()) +
                   context_.userInput().substr(context_.selectedLength());
        }
        return front_ + shown(context_.sentence());
    }
    std::string finished() const override { return front_; }
    void drop_finished() override { front_.clear(); }
    void as_typed() override { typed_ = true; }
    std::vector<std::string> candidates() const override { return shown_; }
    int selected() const override { return selected_; }
    bool empty() const override { return context_.empty(); }
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
            context_.select(0);
            defaults_ |= !chosen_.empty();
            chosen_.push_back(context_.selectedLength());
        }
        if (!context_.selected()) {
            return;
        }
        if (!defaults_) {
            context_.learn(); // The whole sentence, as a word of its own if picked in parts.
        } else {
            // Only the words picked are remembered, without the sentence:
            // an accepted 马 after 来 and 一局 must not come first next time.
            const auto words = context_.selectedWords();
            std::vector<std::string> picked;
            for (std::size_t i = 0; i < words.size() && i < picked_.size(); ++i) {
                if (picked_[i]) {
                    picked.push_back(words[i]);
                }
            }
            if (picked.empty()) {
                return;
            }
            library_->ime->model()->history().add(picked);
        }
        library_->save();
    }

  private:
    std::shared_ptr<Library> library_; // Before context_, which uses its IME.
    libime::PinyinContext context_;
    bool traditional_;
    int selected_{};
    std::vector<std::size_t> chosen_; // selectedLength() before each choice.
    std::vector<bool> picked_;        // Each selected word: picked, not the top candidate.
    bool defaults_{};                 // A top candidate was taken after an earlier choice.
    bool typed_{};                    // Enter: the rest goes as typed (as_typed()).
    std::string front_;               // Sent ahead of a long reading (finished()).
    // Candidates as shown, Traditional ones once each, and libime's index of each.
    std::vector<std::string> shown_;
    std::vector<std::size_t> source_;

    std::string shown(const std::string& text) const {
        return traditional_ && !text.empty() ? library_->to_traditional(text) : text;
    }
    void refresh() {
        shown_.clear();
        source_.clear();
        selected_ = 0;
        typed_ = false;
        const auto& candidates = context_.candidates();
        std::vector<std::pair<std::string, std::size_t>> rows;
        for (std::size_t i = 0; i < candidates.size() && rows.size() < max_candidates; ++i) {
            auto text = shown(candidates[i].toString());
            if (std::none_of(rows.begin(), rows.end(),
                             [&](const auto& row) { return row.first == text; })) {
                rows.emplace_back(std::move(text), i);
            }
        }
        // A rare single character waits behind the rest: neng offers 能 and
        // its common homophones before 㲌. The conversion stays first.
        if (rows.size() > 1) {
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
        if (candidates.empty() || candidates.front().sentence().empty()) {
            throw CjkMessage("ime.commit-first"); // Nothing converts: it must be corrected.
        }
        const auto* first = candidates.front().sentence().front();
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
        front_ += shown(context_.selectedSentence());
        const auto rest = context_.userInput().substr(context_.selectedLength());
        context_.clear();
        chosen_.clear();
        picked_.clear();
        defaults_ = false;
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

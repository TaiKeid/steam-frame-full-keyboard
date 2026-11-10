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
// As PyZy's adapter: a longer reading must be committed first.
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
            throw CjkMessage("ime.commit-first");
        }
        context_.type(std::string(1, static_cast<char>(key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key)));
        refresh();
    }
    void backspace() override {
        if (context_.size() > context_.selectedLength()) {
            context_.backspace();
        } else if (!chosen_.empty()) {
            // Nothing left to delete: the last choice is undone instead.
            context_.cancelTill(chosen_.back());
            chosen_.pop_back();
        }
        refresh();
    }
    void cancel() override {
        context_.clear();
        chosen_.clear();
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
        chosen_.push_back(context_.selectedLength());
        context_.select(source_[static_cast<std::size_t>(index)]);
        refresh();
    }
    std::string preedit() const override {
        return shown(context_.selectedSentence()) + context_.userInput().substr(context_.selectedLength());
    }
    std::string text() const override { return shown(context_.sentence()); }
    std::vector<std::string> candidates() const override { return shown_; }
    int selected() const override { return selected_; }
    bool empty() const override { return context_.empty(); }
    bool has_reading() const override { return context_.size() > context_.selectedLength(); }
    bool chinese() const override { return true; }
    void committed(bool learn) override {
        if (!learn || library_->learning.empty()) {
            return;
        }
        // Enter took the top candidate for what was left; learn it as chosen.
        for (std::size_t i = 0; i < max_keys && !context_.selected() && !context_.candidates().empty(); ++i) {
            context_.select(0);
        }
        if (context_.selected()) {
            context_.learn();
            library_->save();
        }
    }

  private:
    std::shared_ptr<Library> library_; // Before context_, which uses its IME.
    libime::PinyinContext context_;
    bool traditional_;
    int selected_{};
    std::vector<std::size_t> chosen_; // selectedLength() before each choice.
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
        const auto& candidates = context_.candidates();
        for (std::size_t i = 0; i < candidates.size() && shown_.size() < max_candidates; ++i) {
            auto text = shown(candidates[i].toString());
            if (std::find(shown_.begin(), shown_.end(), text) == shown_.end()) {
                shown_.push_back(std::move(text));
                source_.push_back(i);
            }
        }
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

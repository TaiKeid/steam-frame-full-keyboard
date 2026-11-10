// Zhuyin with McBopomofo's engine: its reading grid and Taiwan phrase data, on
// the standard Zhuyin keyboard. Space is the first tone of an open syllable.
// It learns the user's picks with McBopomofo's own user override model.
#include "cjk_engine.hpp"
#include "cjk_module.hpp"
#include "framekeyboard/private_file.hpp"

#include "Engine/Mandarin/Mandarin.h"
#include "Engine/McBopomofoLM.h"
#include "Engine/UserOverrideModel.h"
#include "Engine/gramambular2/reading_grid.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace framekeyboard {
namespace {
namespace fs = std::filesystem;
namespace gram = Formosa::Gramambular2;
using Formosa::Mandarin::BopomofoKeyboardLayout;
using Formosa::Mandarin::BopomofoReadingBuffer;
// As PyZy's adapter: a longer reading must be committed first.
constexpr std::size_t max_syllables = 30;
constexpr std::size_t max_candidates = 50;
// Syllables are joined so in the phrase data: ㄖㄨㄢˇ-ㄊㄧˇ.
constexpr const char* separator = "-";
// McBopomofo's own: how many contexts the model keeps, and a pick's score
// below which the model isn't told (KeyHandler.cpp).
constexpr std::size_t override_capacity = 500;
constexpr double no_override_below = -8.0;
// McBopomofo lets a pick fade with a half-life of 1.5 hours, in memory only.
// Full Keyboard keeps picks across restarts, as it does for Japanese, so they
// fade over weeks instead.
constexpr double override_half_life = 7 * 24 * 3600.0;
// The commits kept for replaying at the next start, newest last.
constexpr std::size_t saved_commits = 500;

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// A candidate the user picked: where it starts, among the syllables, and what.
struct Pick {
    std::size_t start{};
    std::string reading, value;
};

// The phrase data, loaded once and shared by every context.
std::shared_ptr<McBopomofo::McBopomofoLM> language_model() {
    static std::weak_ptr<McBopomofo::McBopomofoLM> cached;
    auto model = cached.lock();
    if (!model) {
        model = std::make_shared<McBopomofo::McBopomofoLM>();
        model->loadLanguageModel((engine_data_dir() / "mcbopomofo/data.txt").c_str());
        if (!model->isDataModelLoaded()) {
            throw CjkMessage("ime.pinyin-dictionary-unavailable");
        }
        cached = model;
    }
    return model;
}

// What was learned, for each learning folder: the user override model, and
// the commits it learned from, saved in picks.tsv as `time<TAB>syllables`
// then `start<TAB>reading<TAB>value` for each pick. At the next start they are
// replayed, at their own times, so the model is as it was.
struct Learning {
    fs::path folder; // Empty, or after a saved file failed to load: nothing is saved.
    std::shared_ptr<McBopomofo::McBopomofoLM> model;
    McBopomofo::UserOverrideModel overrides{override_capacity, override_half_life};
    std::deque<std::string> commits;

    Learning(const fs::path& dir, std::shared_ptr<McBopomofo::McBopomofoLM> phrases)
        : folder(dir), model(std::move(phrases)) {
        if (folder.empty()) {
            return;
        }
        std::ifstream in(folder / "picks.tsv");
        for (std::string line; std::getline(in, line);) {
            if (!replay_line(line)) {
                folder.clear(); // Left as it is, never overwritten.
                return;
            }
            commits.push_back(line);
        }
        while (commits.size() > saved_commits) {
            commits.pop_front();
        }
    }
    // Picks teach the model as McBopomofo's key handler does: the walk before
    // each pick against the one after it, on a grid of their own.
    void learn(const std::vector<std::string>& syllables, const std::vector<Pick>& picks, double time) {
        gram::ReadingGrid grid(model);
        grid.setReadingSeparator(separator);
        for (const auto& syllable : syllables) {
            grid.setCursor(grid.length());
            grid.insertReading(syllable);
        }
        auto walk = grid.walk();
        for (const auto& pick : picks) {
            const auto before = walk.copyWithFixedNodes();
            if (!grid.overrideCandidate(pick.start, gram::ReadingGrid::Candidate(pick.reading, pick.value))) {
                continue;
            }
            walk = grid.walk();
            const auto node = walk.findNodeAt(pick.start);
            if (node != walk.nodes.cend() && (*node)->currentUnigram().score() > no_override_below) {
                overrides.observe(before, walk, pick.start, time);
            }
        }
    }
    void remember(const std::vector<std::string>& syllables, const std::vector<Pick>& picks) {
        const double time = now();
        learn(syllables, picks, time);
        if (folder.empty() || !make_learning_dir(folder)) {
            return;
        }
        std::ostringstream line;
        line.precision(17);
        line << time << '\t';
        for (std::size_t i = 0; i < syllables.size(); ++i) {
            line << (i ? " " : "") << syllables[i];
        }
        for (const auto& pick : picks) {
            line << '\t' << pick.start << '\t' << pick.reading << '\t' << pick.value;
        }
        commits.push_back(line.str());
        while (commits.size() > saved_commits) {
            commits.pop_front();
        }
        std::string text;
        for (const auto& commit : commits) {
            text += commit + '\n';
        }
        write_private_file(folder / "picks.tsv", text);
    }

  private:
    bool replay_line(const std::string& line) {
        std::vector<std::string> fields;
        std::istringstream in(line);
        for (std::string field; std::getline(in, field, '\t');) {
            fields.push_back(field);
        }
        if (fields.size() < 2 || (fields.size() - 2) % 3 != 0) {
            return false;
        }
        try {
            const double time = std::stod(fields[0]);
            std::vector<std::string> syllables;
            std::istringstream words(fields[1]);
            for (std::string syllable; words >> syllable;) {
                syllables.push_back(syllable);
            }
            std::vector<Pick> picks;
            for (std::size_t i = 2; i < fields.size(); i += 3) {
                picks.push_back({std::stoul(fields[i]), fields[i + 1], fields[i + 2]});
            }
            learn(syllables, picks, time);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
};
std::shared_ptr<Learning> learning_for(const fs::path& folder) {
    static std::map<fs::path, std::weak_ptr<Learning>> learnings;
    auto learning = learnings[folder].lock();
    if (!learning) {
        learning = std::make_shared<Learning>(folder, language_model());
        learnings[folder] = learning;
    }
    return learning;
}

class McBopomofoEngine final : public CjkEngine {
  public:
    McBopomofoEngine(const std::string& method, const fs::path& learning_dir)
        : learning_(method == "chinese-zhuyin"
                        ? learning_for(learning_dir.empty() ? fs::path{} : learning_dir / "mcbopomofo")
                        : nullptr),
          reading_(BopomofoKeyboardLayout::StandardLayout()), grid_(learning_ ? learning_->model : nullptr) {
        if (!learning_) {
            throw CjkMessage("ime.engine-missing");
        }
        grid_.setReadingSeparator(separator);
    }
    void type(char key) override {
        if (syllables_.size() + (reading_.isEmpty() ? 0 : 1) >= max_syllables) {
            throw CjkMessage("ime.commit-first");
        }
        if (key == ' ') {
            if (!reading_.isEmpty()) {
                finish_syllable();
            }
        } else if (reading_.isValidKey(key)) {
            reading_.combineKey(key);
            if (reading_.hasToneMarker() && !reading_.hasToneMarkerOnly()) {
                finish_syllable();
            }
        }
        refresh();
    }
    void backspace() override {
        if (!reading_.isEmpty()) {
            reading_.backspace();
        } else if (fixed_ < syllables_.size()) {
            grid_.setCursor(grid_.length());
            grid_.deleteReadingBeforeCursor();
            syllables_.pop_back();
            while (fixed_ > syllables_.size()) {
                fixed_ = chosen_.empty() ? 0 : chosen_.back();
                if (!chosen_.empty()) {
                    chosen_.pop_back();
                }
            }
        } else if (!chosen_.empty()) {
            // Nothing left to delete: the last choice is open again.
            fixed_ = chosen_.back();
            chosen_.pop_back();
        }
        // Picks over what is open again, or deleted, no longer count.
        std::erase_if(picks_, [&](const Pick& pick) { return pick.start >= fixed_; });
        refresh();
    }
    void cancel() override {
        reading_.clear();
        grid_.clear();
        syllables_.clear();
        fixed_ = 0;
        chosen_.clear();
        picks_.clear();
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
        const auto& choice = shown_[static_cast<std::size_t>(index)];
        grid_.overrideCandidate(fixed_, gram::ReadingGrid::Candidate(choice.reading, choice.value));
        picks_.push_back({fixed_, choice.reading, choice.value});
        chosen_.push_back(fixed_);
        fixed_ += choice.length;
        refresh();
    }
    std::string preedit() const override { return text(); }
    // An open syllable goes as its symbols, as typed.
    std::string text() const override { return converted_ + reading_.composedString(); }
    std::vector<std::string> candidates() const override {
        std::vector<std::string> result;
        for (const auto& choice : shown_) {
            result.push_back(choice.value);
        }
        return result;
    }
    int selected() const override { return selected_; }
    bool empty() const override { return syllables_.empty() && reading_.isEmpty(); }
    bool has_reading() const override { return !reading_.isEmpty() || fixed_ < syllables_.size(); }
    bool chinese() const override { return true; }
    bool takes_space() const override { return !reading_.isEmpty(); }
    // The picks of the text that reached the field teach the model.
    void committed(bool learn) override {
        if (learn && !picks_.empty() && !learning_->folder.empty()) {
            learning_->remember(syllables_, picks_);
        }
    }

  private:
    // A phrase for the syllables from fixed_: its reading and how many.
    struct Choice {
        std::string value, reading;
        std::size_t length{};
    };
    std::shared_ptr<Learning> learning_; // Before grid_, which reads its model.
    BopomofoReadingBuffer reading_;      // The open syllable.
    gram::ReadingGrid grid_;
    std::vector<std::string> syllables_; // Finished syllables, as in the grid.
    std::size_t fixed_{};                // Syllables chosen so far.
    std::vector<std::size_t> chosen_;    // fixed_ before each choice.
    std::vector<Pick> picks_;            // This reading's picks, to learn once committed.
    std::string converted_;              // The grid's best walk.
    std::vector<Choice> shown_;
    int selected_{};

    // A syllable the data doesn't know stays open, to be corrected. One it
    // knows may take what the user picked there before, as in McBopomofo.
    void finish_syllable() {
        const auto syllable = reading_.syllable().composedString();
        if (!learning_->model->hasUnigrams(syllable)) {
            return;
        }
        grid_.setCursor(grid_.length());
        grid_.insertReading(syllable);
        syllables_.push_back(syllable);
        reading_.clear();
        const auto last = grid_.length() - 1;
        const auto suggestion = learning_->overrides.suggest(grid_.walk(), last, now());
        if (!suggestion.empty()) {
            grid_.overrideCandidate(last, suggestion.candidate,
                                    suggestion.forceHighScoreOverride
                                        ? gram::ReadingGrid::Node::OverrideType::kOverrideValueWithHighScore
                                        : gram::ReadingGrid::Node::OverrideType::kOverrideValueWithScoreFromTopUnigram);
        }
    }
    // The walk's phrase at fixed_ comes first, so Space keeps it; then every
    // phrase starting there, longest first, each by score.
    void refresh() {
        shown_.clear();
        selected_ = 0;
        converted_.clear();
        const auto walk = grid_.walk();
        std::size_t start = 0;
        for (const auto& node : walk.nodes) {
            converted_ += node->value();
            if (start == fixed_ && reading_.isEmpty()) {
                shown_.push_back({node->value(), node->reading(), node->spanningLength()});
            }
            start += node->spanningLength();
        }
        if (!reading_.isEmpty() || fixed_ >= syllables_.size()) {
            shown_.clear();
            return;
        }
        const auto longest = std::min(syllables_.size() - fixed_, gram::ReadingGrid::kMaximumSpanLength);
        for (std::size_t length = longest; length > 0 && shown_.size() < max_candidates; --length) {
            std::string reading;
            for (std::size_t i = fixed_; i < fixed_ + length; ++i) {
                reading += (i > fixed_ ? separator : "") + syllables_[i];
            }
            auto unigrams = learning_->model->getUnigrams(reading);
            std::stable_sort(unigrams.begin(), unigrams.end(),
                             [](const auto& a, const auto& b) { return a.score() > b.score(); });
            for (const auto& unigram : unigrams) {
                // Macros (dates and the like) need McBopomofo's front end.
                if (unigram.value().starts_with("MACRO@") || shown_.size() >= max_candidates ||
                    std::any_of(shown_.begin(), shown_.end(),
                                [&](const Choice& choice) { return choice.value == unigram.value(); })) {
                    continue;
                }
                shown_.push_back({unigram.value(), reading, length});
            }
        }
    }
};
} // namespace
} // namespace framekeyboard

// The only exported entry point. The module and host ship in the same release
// and share this internal C++ interface.
extern "C" __attribute__((visibility("default"))) framekeyboard::CjkEngine*
framekeyboard_create_cjk_v3(const char* method, const char* learning_dir) {
    return new framekeyboard::McBopomofoEngine(method, learning_dir ? learning_dir : "");
}

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
// A longer reading sends its converted front first, as McBopomofo does, and
// goes on.
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
            if (!grid.overrideCandidate(pick.start,
                                        gram::ReadingGrid::Candidate(pick.reading, pick.value))) {
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

// The reading converts as it is typed, and a cursor between its syllables
// picks the phrase the candidates are for, as in McBopomofo and Microsoft's
// New Phonetic: it starts at the end and the arrows move it. Space and ↓
// start choosing for that phrase (the app's keys), and a pick replaces it and
// leaves the reading open. Only Enter sends the line.
class McBopomofoEngine final : public CjkEngine {
  public:
    McBopomofoEngine(const std::string& method, const fs::path& learning_dir)
        : learning_(method == "chinese-zhuyin"
                        ? learning_for(learning_dir.empty() ? fs::path{} : learning_dir / "mcbopomofo")
                        : nullptr),
          reading_(BopomofoKeyboardLayout::StandardLayout()),
          grid_(learning_ ? learning_->model : nullptr) {
        if (!learning_) {
            throw CjkMessage("ime.engine-missing");
        }
        grid_.setReadingSeparator(separator);
    }
    void type(char key) override {
        if (syllables_.size() + (reading_.isEmpty() ? 0 : 1) >= max_syllables) {
            send_front();
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
    // The open syllable's last key, else the syllable before the cursor.
    void backspace() override {
        if (!reading_.isEmpty()) {
            reading_.backspace();
        } else if (cursor_ > 0) {
            remove(cursor_ - 1);
        }
        refresh();
    }
    void delete_forward() override {
        if (reading_.isEmpty() && cursor_ < syllables_.size()) {
            remove(cursor_);
            refresh();
        }
    }
    // Esc takes one step: the open syllable, then the whole reading.
    void cancel_open() override {
        if (reading_.isEmpty()) {
            cancel();
        } else {
            reading_.clear();
            refresh();
        }
    }
    void cancel() override {
        reading_.clear();
        grid_.clear();
        syllables_.clear();
        cursor_ = 0;
        picks_.clear();
        front_.clear();
        refresh();
    }
    void cycle(int direction) override {
        if (const int count = static_cast<int>(shown_.size())) {
            selected_ = (selected_ + direction % count + count) % count;
        }
    }
    // A pick replaces the phrase at the cursor; the reading stays open, and so
    // does the cursor where it is, as McBopomofo's default.
    void choose(int index) override {
        if (index < 0 || index >= static_cast<int>(shown_.size())) {
            return;
        }
        const auto choice = shown_[static_cast<std::size_t>(index)];
        const auto at = active();
        if (grid_.overrideCandidate(at, gram::ReadingGrid::Candidate(choice.reading, choice.value)) &&
            index > 0) {
            picks_.push_back({at, choice.reading, choice.value});
        }
        refresh();
    }
    std::string preedit() const override { return text(); }
    // An open syllable goes as its symbols, as typed, at the cursor.
    std::string text() const override {
        const auto at = caret_byte();
        return front_ + converted_.substr(0, at) + reading_.composedString() + converted_.substr(at);
    }
    std::vector<std::string> candidates() const override {
        std::vector<std::string> result;
        for (const auto& choice : shown_) {
            result.push_back(choice.value);
        }
        return result;
    }
    int selected() const override { return selected_; }
    bool empty() const override { return syllables_.empty() && reading_.isEmpty(); }
    // A pick never ends the reading: only Enter sends it.
    bool has_reading() const override { return !empty(); }
    bool chinese() const override { return true; }
    bool takes_space() const override { return !reading_.isEmpty(); }
    std::string finished() const override { return front_; }
    void drop_finished() override { front_.clear(); }
    bool has_cursor() const override { return true; }
    // An open syllable keeps the cursor where it is, as in McBopomofo.
    void move(int syllables) override {
        if (!reading_.isEmpty()) {
            return;
        }
        const auto to = syllables < 0
                            ? cursor_ - std::min(cursor_, static_cast<std::size_t>(-syllables))
                            : std::min(syllables_.size(), cursor_ + static_cast<std::size_t>(syllables));
        if (to != cursor_) {
            cursor_ = to;
            refresh();
        }
    }
    bool choosing() const override { return choosing_; }
    void set_choosing(bool choosing) override {
        choosing_ = choosing && !shown_.empty();
        selected_ = 0;
    }
    // McBopomofo's punctuation readings for the standard layout, as
    // _punctuation_Standard_< for ，, or else its common ones; the text the
    // language gives is the one shown, where the reading offers it.
    bool insert_symbol(char symbol, const std::string& text) override {
        if (syllables_.size() + 1 >= max_syllables) {
            send_front();
        }
        if (!reading_.isEmpty()) {
            finish_syllable(); // Its first tone, as Space gives it.
            if (!reading_.isEmpty()) {
                return false;
            }
        }
        for (const auto& reading :
             {std::string("_punctuation_Standard_") + symbol, std::string("_punctuation_") + symbol}) {
            if (!learning_->model->hasUnigrams(reading)) {
                continue;
            }
            const auto at = insert(reading);
            if (!grid_.overrideCandidate(at, gram::ReadingGrid::Candidate(reading, text))) {
                remove(at);
                break;
            }
            refresh();
            return true;
        }
        refresh();
        return false;
    }
    bool highlight(std::size_t& begin, std::size_t& end, std::size_t& caret) const override {
        if (syllables_.empty() || !reading_.isEmpty()) {
            return false;
        }
        begin = front_.size() + active_begin_;
        end = front_.size() + active_end_;
        caret = front_.size() + caret_byte();
        return true;
    }
    // The picks of the text that reached the field teach the model.
    void committed(bool learn) override {
        if (learn && !picks_.empty() && !learning_->folder.empty()) {
            learning_->remember(syllables_, picks_);
        }
    }

  private:
    // A phrase over the active syllable: its reading and value.
    struct Choice {
        std::string value, reading;
    };
    std::shared_ptr<Learning> learning_; // Before grid_, which reads its model.
    BopomofoReadingBuffer reading_;      // The open syllable, at the cursor.
    gram::ReadingGrid grid_;
    std::vector<std::string> syllables_; // Finished syllables, as in the grid.
    std::size_t cursor_{};               // Syllables before the cursor.
    std::vector<Pick> picks_;            // This reading's picks, to learn once committed.
    bool choosing_{};                    // Choosing for the phrase at the cursor.
    std::string converted_;              // The grid's best walk.
    // Bytes of converted_: the active phrase, and each syllable boundary the
    // cursor can stand on (none inside a phrase whose text isn't a
    // character a syllable).
    std::size_t active_begin_{}, active_end_{};
    std::vector<std::size_t> boundaries_;
    std::string front_; // Sent ahead of a long reading (finished()).
    std::vector<Choice> shown_;
    int selected_{};

    // The syllable the candidates are for: the one before the cursor, or the
    // first with the cursor at the start.
    std::size_t active() const { return cursor_ > 0 ? cursor_ - 1 : 0; }
    std::size_t caret_byte() const {
        return cursor_ < boundaries_.size() ? boundaries_[cursor_] : converted_.size();
    }
    // A syllable the data doesn't know stays open, to be corrected. One it
    // knows goes in at the cursor, and may take what the user picked there
    // before, as in McBopomofo.
    void finish_syllable() {
        const auto syllable = reading_.syllable().composedString();
        if (!learning_->model->hasUnigrams(syllable)) {
            return;
        }
        const auto at = insert(syllable);
        reading_.clear();
        const auto suggestion = learning_->overrides.suggest(grid_.walk(), at, now());
        if (!suggestion.empty()) {
            grid_.overrideCandidate(
                at, suggestion.candidate,
                suggestion.forceHighScoreOverride
                    ? gram::ReadingGrid::Node::OverrideType::kOverrideValueWithHighScore
                    : gram::ReadingGrid::Node::OverrideType::kOverrideValueWithScoreFromTopUnigram);
        }
    }
    // A reading the data knows goes in at the cursor, which moves past it;
    // picks after it move along. Returns where it went.
    std::size_t insert(const std::string& reading) {
        grid_.setCursor(cursor_);
        grid_.insertReading(reading);
        syllables_.insert(syllables_.begin() + static_cast<std::ptrdiff_t>(cursor_), reading);
        for (auto& pick : picks_) {
            pick.start += pick.start >= cursor_ ? 1 : 0;
        }
        return cursor_++;
    }
    // Takes out the syllable at `at`, before the cursor or at it; a pick over
    // it no longer counts.
    void remove(std::size_t at) {
        grid_.setCursor(at + 1);
        grid_.deleteReadingBeforeCursor();
        syllables_.erase(syllables_.begin() + static_cast<std::ptrdiff_t>(at));
        std::erase_if(picks_, [&](const Pick& pick) { return pick.start == at; });
        for (auto& pick : picks_) {
            pick.start -= pick.start > at ? 1 : 0;
        }
        cursor_ -= cursor_ > at ? 1 : 0;
    }
    // The reading is full: the walk's first phrase goes ahead, to the field
    // (finished()), and leaves the grid. Picks in it are not learned.
    void send_front() {
        const auto walk = grid_.walk();
        if (walk.nodes.empty()) {
            throw CjkMessage("ime.commit-first");
        }
        const auto length = walk.nodes.front()->spanningLength();
        front_ += walk.nodes.front()->value();
        grid_.setCursor(length);
        for (std::size_t i = 0; i < length; ++i) {
            grid_.deleteReadingBeforeCursor();
        }
        syllables_.erase(syllables_.begin(), syllables_.begin() + static_cast<std::ptrdiff_t>(length));
        std::erase_if(picks_, [&](const Pick& pick) { return pick.start < length; });
        for (auto& pick : picks_) {
            pick.start -= length;
        }
        cursor_ = cursor_ > length ? cursor_ - length : 0;
        refresh();
    }
    // The walk's phrase over the active syllable comes first, where choosing
    // starts; then every phrase over it, longest first, each by score.
    void refresh() {
        shown_.clear();
        selected_ = 0;
        choosing_ = false;
        converted_.clear();
        boundaries_.assign(1, 0);
        active_begin_ = active_end_ = 0;
        const auto walk = grid_.walk();
        const auto at = active();
        std::size_t start = 0;
        for (const auto& node : walk.nodes) {
            const auto& value = node->value();
            const auto length = node->spanningLength();
            const bool covers = at >= start && at < start + length;
            if (covers) {
                active_begin_ = converted_.size();
                active_end_ = converted_.size() + value.size();
                if (reading_.isEmpty()) {
                    shown_.push_back({value, node->reading()});
                }
            }
            // A phrase of one character a syllable has a boundary after each;
            // another only at its end.
            const bool even = utf8_length(value) == length;
            for (std::size_t i = 1; i <= length; ++i) {
                boundaries_.push_back(converted_.size() + (even ? utf8_prefix(value, i) : value.size()));
            }
            converted_ += value;
            start += length;
        }
        if (!reading_.isEmpty() || syllables_.empty()) {
            shown_.clear();
            return;
        }
        const auto count = syllables_.size();
        const auto longest = std::min(count, gram::ReadingGrid::kMaximumSpanLength);
        for (std::size_t length = longest; length > 0 && shown_.size() < max_candidates; --length) {
            for (std::size_t first = at + 1 >= length ? at + 1 - length : 0;
                 first <= at && first + length <= count; ++first) {
                std::string reading;
                for (std::size_t i = first; i < first + length; ++i) {
                    reading += (i > first ? separator : "") + syllables_[i];
                }
                auto unigrams = learning_->model->getUnigrams(reading);
                std::stable_sort(unigrams.begin(), unigrams.end(),
                                 [](const auto& a, const auto& b) { return a.score() > b.score(); });
                for (const auto& unigram : unigrams) {
                    // Macros (dates and the like) need McBopomofo's front end.
                    if (unigram.value().starts_with("MACRO@") || shown_.size() >= max_candidates ||
                        std::any_of(shown_.begin(), shown_.end(), [&](const Choice& choice) {
                            return choice.value == unigram.value();
                        })) {
                        continue;
                    }
                    shown_.push_back({unigram.value(), reading});
                }
            }
        }
        // A rare single character waits behind the rest: ㄨㄛˇ offers 我 婐 捰
        // before 䂺. The conversion's own phrase stays first.
        if (shown_.size() > 1) {
            std::stable_partition(shown_.begin() + 1, shown_.end(), [](const Choice& choice) {
                return utf8_length(choice.value) > 1 || !rare_character(choice.value);
            });
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

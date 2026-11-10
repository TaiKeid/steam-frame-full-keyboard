// Zhuyin with McBopomofo's engine: its reading grid and Taiwan phrase data, on
// the standard Zhuyin keyboard. Space is the first tone of an open syllable.
// It learns the user's picks with McBopomofo's own user override model.
#include "cjk_engine.hpp"
#include "cjk_module.hpp"
#include "framekeyboard/private_file.hpp"
#include "pinyin_reranker.hpp"

#include "Engine/Mandarin/Mandarin.h"
#include "Engine/McBopomofoLM.h"
#include "Engine/UserOverrideModel.h"
#include "Engine/gramambular2/reading_grid.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
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
// A candidate the user picked: its anchor syllable, reading and value.
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
        const auto* hybrid = std::getenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES");
        const auto* model = std::getenv("FRAMEKEYBOARD_ZHUYIN_RERANK_MODEL");
        if (hybrid && std::string_view(hybrid) == "1" && model && *model) {
            reranker_ = PinyinReranker::load(model, "FKZYR001");
        }
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
        } else if (syllables_.empty() && !front_.empty()) {
            front_.resize(utf8_prefix(front_, utf8_length(front_) - 1));
        }
        refresh();
    }
    void delete_forward() override {
        if (reading_.isEmpty() && cursor_ < syllables_.size()) {
            remove(cursor_);
            refresh();
        }
    }
    // Esc takes one step: the open syllable, then the reading, keeping
    // front text the field refused for the next commit (R149), then that
    // text (R156).
    void cancel_open() override {
        if (!reading_.isEmpty()) {
            reading_.clear();
            refresh();
        } else if (syllables_.empty()) {
            cancel();
        } else {
            auto front = std::move(front_);
            cancel();
            front_ = std::move(front);
            refresh();
        }
    }
    void cancel() override {
        reading_.clear();
        grid_.clear();
        syllables_.clear();
        cursor_ = 0;
        fixed_.clear();
        preferred_.clear();
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
        if (grid_.overrideCandidate(at, gram::ReadingGrid::Candidate(choice.reading, choice.value))) {
            fix(at, choice.reading, choice.value,
                gram::ReadingGrid::Node::OverrideType::kOverrideValueWithHighScore, index > 0);
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
    bool empty() const override { return syllables_.empty() && reading_.isEmpty() && front_.empty(); }
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
            // Checked before insert(), which drops the picks of a phrase it
            // splits: a refused override would lose them for nothing (R148).
            const auto unigrams = learning_->model->getUnigrams(reading);
            if (std::none_of(unigrams.begin(), unigrams.end(),
                             [&](const auto& unigram) { return unigram.value() == text; })) {
                continue;
            }
            const auto at = insert(reading);
            if (!grid_.overrideCandidate(at, gram::ReadingGrid::Candidate(reading, text))) {
                remove(at);
                break;
            }
            fix(at, reading, text, gram::ReadingGrid::Node::OverrideType::kOverrideValueWithHighScore);
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
    // Toneless Pinyin for each run of syllables between punctuation.
    std::vector<std::string> pinyin_runs() const override {
        std::vector<std::string> runs;
        for (const auto& [start, length] : runs_()) {
            std::string pinyin;
            for (std::size_t i = start; i < start + length; ++i) {
                pinyin += (i > start ? "'" : "") +
                          Formosa::Mandarin::BopomofoSyllable::FromComposedString(syllables_[i])
                              .HanyuPinyinString(false, true);
            }
            runs.push_back(pinyin);
        }
        return runs;
    }
    // For each run, the proposed sentence that fits every typed syllable and
    // scores best in McBopomofo's phrase data, less one for each place down
    // the proposals (tuned on benchmark set 3); a run with none keeps
    // McBopomofo's own. It is pinned where nothing was picked or learned.
    void prefer(const std::vector<std::vector<std::string>>& sentences) override {
        preferred_.clear();
        const auto runs = runs_();
        // Never feed the previous proposed sentence back as the native own
        // hypothesis. Take one walk with only user/learned overrides applied.
        std::vector<std::optional<std::string>> own;
        if (reranker_) {
            own.resize(runs.size());
            rebuild();
            const auto walk = grid_.walk();
            for (std::size_t r = 0; r < runs.size(); ++r) {
                const auto [start, length] = runs[r];
                std::string value;
                std::size_t begin = 0, covered = 0;
                bool safe = true;
                for (const auto& node : walk.nodes) {
                    const auto end = begin + node->spanningLength();
                    if (begin < start + length && start < end) {
                        if (begin < start || end > start + length) {
                            safe = false;
                            break;
                        }
                        value += node->value();
                        covered += node->spanningLength();
                    }
                    begin = end;
                }
                if (safe && covered == length) {
                    own[r] = std::move(value);
                }
            }
        }
        for (std::size_t r = 0; r < runs.size() && r < sentences.size(); ++r) {
            const auto [start, length] = runs[r];
            std::vector<Fixed> best;
            double best_score = -1e18;
            const auto& proposals = sentences[r];
            std::vector<Fixed> own_segments;
            double own_fit = 0;
            const bool protected_run = std::any_of(fixed_.begin(), fixed_.end(), [&](const Fixed& f) {
                return f.begin < start + length && start < f.begin + f.length;
            });
            if (reranker_ && own[r] && !protected_run) {
                own_fit = fit(*own[r], start, length, own_segments);
            }
            const bool residual = !own_segments.empty();
            std::optional<double> base;
            std::vector<std::string_view> seen;
            for (std::size_t rank = 0; rank < proposals.size(); ++rank) {
                if (residual && rank >= max_candidates) {
                    break;
                }
                if (residual && std::find(seen.begin(), seen.end(), proposals[rank]) != seen.end()) {
                    continue;
                }
                if (residual) {
                    seen.push_back(proposals[rank]);
                }
                std::vector<Fixed> segments;
                double score = fit(proposals[rank], start, length, segments);
                if (segments.empty()) {
                    continue;
                }
                score -= static_cast<double>(rank);
                if (residual) {
                    if (!base) {
                        base = score;
                    }
                    score = reranker_->score(proposals[rank], {}, rank, score, *base);
                }
                if (score > best_score) {
                    best_score = score;
                    best = std::move(segments);
                }
            }
            if (residual && std::find(seen.begin(), seen.end(), *own[r]) == seen.end()) {
                const double heuristic = own_fit - 20.;
                const double score =
                    reranker_->score(*own[r], {}, 20, heuristic, base.value_or(heuristic), true);
                if (score > best_score) {
                    best.clear(); // Own wins by leaving this run's native walk unforced.
                }
            }
            preferred_.insert(preferred_.end(), best.begin(), best.end());
        }
        rebuild();
        refresh();
    }
    // The picks of the text that reached the field teach the model.
    void committed(bool learn) override {
        if (!learn || learning_->folder.empty()) {
            return;
        }
        // The display and learning share override ownership. Editing or
        // replacing a phrase must remove its learning intent at the same time.
        std::vector<Pick> picks;
        for (const auto& fixed : fixed_) {
            if (fixed.picked) {
                picks.push_back({fixed.at, fixed.reading, fixed.value});
            }
        }
        if (!picks.empty()) {
            learning_->remember(syllables_, picks);
        }
    }

  private:
    // A phrase over the active syllable: its reading and value.
    struct Choice {
        std::string value, reading;
    };
    std::shared_ptr<Learning> learning_; // Before grid_, which reads its model.
    std::optional<PinyinReranker> reranker_;
    BopomofoReadingBuffer reading_; // The open syllable, at the cursor.
    gram::ReadingGrid grid_;
    std::vector<std::string> syllables_; // Finished syllables, as in the grid.
    std::size_t cursor_{};               // Syllables before the cursor.
    // An override of the grid: where it was made, the phrase it pinned (its
    // first syllable and length), and what. fixed_ holds the user's picks,
    // learned suggestions and punctuation; preferred_ the proposed sentence
    // (prefer()), which yields to them. rebuild() makes the grid again from them.
    struct Fixed {
        std::size_t at{}, begin{}, length{};
        std::string reading, value;
        gram::ReadingGrid::Node::OverrideType type{};
        bool picked{}; // An explicit non-default choice, learned only if it survives until commit.
    };
    std::vector<Fixed> fixed_, preferred_;
    bool choosing_{};       // Choosing for the phrase at the cursor.
    std::string converted_; // The grid's best walk.
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
        // Learned suggestions are looked up in McBopomofo's own context, as
        // they were learned; a proposed sentence comes back after (prefer()).
        if (!preferred_.empty()) {
            preferred_.clear();
            rebuild();
        }
        const auto at = insert(syllable);
        reading_.clear();
        const auto suggestion = learning_->overrides.suggest(grid_.walk(), at, now());
        if (!suggestion.empty()) {
            const auto type =
                suggestion.forceHighScoreOverride
                    ? gram::ReadingGrid::Node::OverrideType::kOverrideValueWithHighScore
                    : gram::ReadingGrid::Node::OverrideType::kOverrideValueWithScoreFromTopUnigram;
            if (grid_.overrideCandidate(at, suggestion.candidate, type)) {
                fix(at, {}, suggestion.candidate, type); // By value alone, as suggested.
            }
        }
    }
    // Records an override just made at `at`, with the phrase it pinned.
    void fix(std::size_t at, const std::string& reading, const std::string& value,
             gram::ReadingGrid::Node::OverrideType type, bool picked = false) {
        std::size_t begin = 0;
        for (const auto& node : grid_.walk().nodes) {
            if (at < begin + node->spanningLength()) {
                // A later override of the same phrase replaces the earlier.
                std::erase_if(fixed_, [&](const Fixed& f) {
                    return f.begin < begin + node->spanningLength() && begin < f.begin + f.length;
                });
                fixed_.push_back({at, begin, node->spanningLength(), reading, value, type, picked});
                return;
            }
            begin += node->spanningLength();
        }
    }
    // Overrides follow a syllable put in at `at` (+1) or taken out (−1); one
    // whose phrase it splits or loses is dropped.
    void shift(std::vector<Fixed>& list, std::size_t at, int change) {
        std::erase_if(list, [&](const Fixed& f) {
            return change > 0 ? f.begin < at && at < f.begin + f.length
                              : f.begin <= at && at < f.begin + f.length;
        });
        for (auto& f : list) {
            if (f.begin >= at + (change > 0 ? 0 : 1)) {
                f.begin += static_cast<std::size_t>(change);
                f.at += static_cast<std::size_t>(change);
            }
        }
    }
    // Runs of syllables between punctuation: (first, length).
    std::vector<std::pair<std::size_t, std::size_t>> runs_() const {
        std::vector<std::pair<std::size_t, std::size_t>> runs;
        for (std::size_t i = 0; i < syllables_.size();) {
            if (syllables_[i].starts_with("_")) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < syllables_.size() && !syllables_[j].starts_with("_")) {
                ++j;
            }
            runs.emplace_back(i, j - i);
            i = j;
        }
        return runs;
    }
    // How well a sentence fits the run's syllables: its best split into
    // McBopomofo's phrases for them, summing their scores; no segments when
    // a character doesn't have its syllable's reading.
    double fit(const std::string& sentence, std::size_t start, std::size_t length,
               std::vector<Fixed>& segments) const {
        std::vector<std::string> characters;
        for (std::size_t i = 0; i < sentence.size();) {
            const auto bytes = utf8_prefix(std::string_view(sentence).substr(i), 1);
            characters.push_back(sentence.substr(i, bytes));
            i += bytes;
        }
        if (characters.size() != length) {
            return 0;
        }
        struct Step {
            double score;
            std::size_t from;
            std::string reading, value;
        };
        std::vector<std::optional<Step>> best(length + 1);
        best[0] = Step{0, 0, {}, {}};
        for (std::size_t i = 0; i < length; ++i) {
            if (!best[i]) {
                continue;
            }
            std::string reading, value;
            for (std::size_t j = i; j < length && j < i + gram::ReadingGrid::kMaximumSpanLength; ++j) {
                reading += (j > i ? separator : "") + syllables_[start + j];
                value += characters[j];
                for (const auto& unigram : learning_->model->getUnigrams(reading)) {
                    if (unigram.value() == value &&
                        (!best[j + 1] || best[i]->score + unigram.score() > best[j + 1]->score)) {
                        best[j + 1] = Step{best[i]->score + unigram.score(), i, reading, value};
                    }
                }
            }
        }
        if (!best[length]) {
            return 0;
        }
        for (std::size_t j = length; j > 0; j = best[j]->from) {
            const auto from = best[j]->from;
            segments.insert(segments.begin(),
                            Fixed{start + from, start + from, j - from, best[j]->reading, best[j]->value,
                                  gram::ReadingGrid::Node::OverrideType::kOverrideValueWithHighScore});
        }
        return best[length]->score;
    }
    // The grid again from the syllables: the user's overrides first, then
    // the preferred sentence's phrases that none of them touches.
    void rebuild() {
        grid_.clear();
        for (const auto& syllable : syllables_) {
            grid_.setCursor(grid_.length());
            grid_.insertReading(syllable);
        }
        for (const auto& f : fixed_) {
            if (f.reading.empty()) {
                grid_.overrideCandidate(f.at, f.value, f.type);
            } else {
                grid_.overrideCandidate(f.at, gram::ReadingGrid::Candidate(f.reading, f.value), f.type);
            }
        }
        for (const auto& p : preferred_) {
            if (std::none_of(fixed_.begin(), fixed_.end(), [&](const Fixed& f) {
                    return f.begin < p.begin + p.length && p.begin < f.begin + f.length;
                })) {
                grid_.overrideCandidate(p.at, gram::ReadingGrid::Candidate(p.reading, p.value), p.type);
            }
        }
    }
    // A reading the data knows goes in at the cursor, which moves past it;
    // picks after it move along. Returns where it went.
    std::size_t insert(const std::string& reading) {
        grid_.setCursor(cursor_);
        grid_.insertReading(reading);
        syllables_.insert(syllables_.begin() + static_cast<std::ptrdiff_t>(cursor_), reading);
        shift(fixed_, cursor_, 1);
        shift(preferred_, cursor_, 1);
        return cursor_++;
    }
    // Takes out the syllable at `at`, before the cursor or at it; a pick over
    // it no longer counts.
    void remove(std::size_t at) {
        grid_.setCursor(at + 1);
        grid_.deleteReadingBeforeCursor();
        syllables_.erase(syllables_.begin() + static_cast<std::ptrdiff_t>(at));
        shift(fixed_, at, -1);
        shift(preferred_, at, -1);
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
        for (std::size_t i = 0; i < length; ++i) {
            shift(fixed_, 0, -1);
            shift(preferred_, 0, -1);
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

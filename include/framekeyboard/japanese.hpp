#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace framekeyboard {
// Kanji conversion of a reading. The keyboard uses the system's Anthy, loaded
// on first use; tests supply their own.
class JapaneseConverter {
  public:
    struct Segment {
        std::string reading;                 // The segment's kana.
        std::vector<std::string> candidates; // Never empty.
    };
    virtual ~JapaneseConverter() = default;
    // Splits a reading into segments, each with its candidates.
    virtual std::vector<Segment> convert(const std::string& reading) = 0;
    // Lengthens or shortens segment `index` of the last conversion by `delta`
    // characters and splits the reading after it again: every segment, or
    // none if it cannot change. The segments before `index` stay as they are.
    virtual std::vector<Segment> resize(int index, int delta) = 0;
    // Learning (docs/japanese.md, Learning), which a converter may ignore:
    // the phrase just committed from the last conversion, as each segment's
    // chosen text; words committed before that begin with `reading`; and the
    // one of those that was picked.
    virtual void learn(const std::vector<std::string>& chosen) { (void)chosen; }
    virtual std::vector<std::string> predict(const std::string& reading) {
        (void)reading;
        return {};
    }
    virtual void learn_prediction(const std::string& reading, const std::string& text) {
        (void)reading;
        (void)text;
    }
};
// Anthy's learning, process-wide like Anthy itself. Off (previews, tours),
// Anthy keeps no record at all; on, a record of Full Keyboard's own in
// `home`/.anthy, apart from the one the stock keyboard learns into there.
struct AnthyLearning {
    bool operator==(const AnthyLearning&) const = default;
    bool on{};
    // Where Anthy keeps it; empty is default_anthy_home().
    std::filesystem::path home;
};
// Sets the learning Anthy starts with. True when Anthy already runs with
// other learning, which restart_anthy() then applies.
bool configure_anthy(AnthyLearning learning);
// Whether anything in this process configured Anthy yet.
bool anthy_configured();
// Restarts a running Anthy with the configured learning, first deleting what
// it learned if `clear`, running or not. It waits for conversions under way
// and blocks while Anthy loads its dictionary again: a worker thread's job.
void restart_anthy(bool clear);
// Whether Full Keyboard's record holds anything to clear.
bool anthy_learned();
// Starts Full Keyboard's record from a copy of the stock keyboard's, once:
// unless `marker` exists, which it then creates, or there is a record of our
// own already. It reads the stock record under Anthy's lock and leaves it as
// it is. True when it copied anything. Call before Anthy starts.
bool import_stock_learning(const std::filesystem::path& marker);
// The user's home, as other programs find it: $HOME, else the password database.
std::filesystem::path default_anthy_home();
// A dictionary other than the installed one, for the simulator and tests;
// every start of Anthy, restarts included, loads it. Before the first conversion.
void set_anthy_dictionary(const std::filesystem::path& dictionary);
// Kana in hiragana or katakana; other characters stay.
std::string kana_in_script(const std::string& text, bool katakana);
// The common words a partly typed reading completes to (japanese/words.tsv;
// docs/japanese.md, Prediction), read on first use. An empty path has none.
class JapaneseWords {
  public:
    explicit JapaneseWords(std::filesystem::path file = {}) : file_(std::move(file)) {}
    // Up to `most` words whose reading begins with `reading` and is longer,
    // commonest first, never one turning its last kana into another sound
    // with a small kana (きみょう for きみ). `first` says whether they come
    // before the reading's own conversion: when it is no word itself, nor a
    // word and a particle (きょうは), or a clearly rarer one of three kana or
    // more than the commonest of them (おめでと, before おめでとう).
    std::vector<std::string> complete(const std::string& reading, std::size_t most, bool& first) const;
    // complete() with each word's cost, lower is commoner.
    std::vector<std::pair<std::string, int>> completions(const std::string& reading, std::size_t most,
                                                         bool& first) const;
    // Whether a completion of this cost leads the reading's own conversion,
    // by complete()'s rule but for an adjective's stem.
    bool completion_leads(const std::string& reading, int cost) const;
    // The reading's lowest cost as a word, or as a word and a particle a
    // little more; the largest int when it is neither.
    int own_cost(const std::string& reading) const;
    // Up to `most` words of exactly this reading, lowest cost first.
    std::vector<std::string> exact(const std::string& reading, std::size_t most) const;

  private:
    struct Entry {
        std::uint32_t reading{}, word{}; // Offsets into text_.
        std::uint16_t reading_size{}, word_size{}, cost{};
    };
    void load() const;
    std::filesystem::path file_;
    mutable bool loaded_{};
    mutable std::string text_;
    mutable std::vector<Entry> entries_; // By reading, as the file is.
};
// Pure local preedit state. Nothing reaches an application until explicit commit.
class JapaneseComposer {
  public:
    using Factory = std::function<std::unique_ptr<JapaneseConverter>()>;
    // Without a factory, conversion loads Anthy.
    explicit JapaneseComposer(Factory factory = {});
    ~JapaneseComposer();
    void roman(char key);
    void kana(const std::string& text);
    // Changes the last kana: ゛ voices it, ゜ makes it semi-voiced, 小 makes it
    // small, each toggling back; ゛゜小 cycles through its variants (つ っ づ).
    // False when the last character has no such form.
    bool mark(const std::string& mark);
    // The reading character before the cursor, or empty while romaji is
    // pending or converting.
    std::string last_kana() const;
    // Removes the character before the cursor (or a pending roman letter).
    void backspace();
    // Moves the cursor in the reading by a character, before conversion;
    // typing and the marks act there. False at an end, or while converting.
    bool move(int direction);
    // The cursor's byte offset in preedit(), before conversion.
    std::size_t cursor() const;
    void convert();
    void cycle(int direction);
    void segment(int direction);
    // Moves the end of the active segment one character later (1) or earlier
    // (-1); the converter splits the rest of the reading again.
    void resize(int direction);
    // Chooses a candidate of the active segment and moves on to the next one.
    // True when it was the last segment: the phrase is ready to commit.
    bool choose(int index);
    // Live candidates, offered before conversion: the whole phrase converted,
    // with learning the words committed before that begin with the reading,
    // the first segment's candidates, then the reading in hiragana and in
    // katakana, then emoji for those words, each once. prepare_live() converts the reading for them;
    // they stay listed, stale, once it changes, until prepared again.
    void prepare_live();
    const std::vector<std::string>& live() const { return live_.texts; }
    bool live_current() const;
    // Picks a live candidate of the current list. True when the phrase is
    // ready to commit; a first segment's candidate of a longer phrase instead
    // starts conversion at the second segment.
    bool choose_live(int index);
    void script(bool katakana);
    void cancel();
    void unconvert();
    std::string preedit(bool mark_active = false) const;
    // Byte range of the active segment in preedit(); empty while not converting.
    std::pair<std::size_t, std::size_t> active_range() const;
    std::string commit_text();
    // Learning, off unless the user turned it on and the field may keep what
    // is typed in it.
    void set_learning(bool on) { learning_ = on; }
    // After commit_text() reached the field: teaches the converter the phrase.
    void learn();
    // Emoji for words, which live candidates offer last (ねこ, 猫 → 🐈).
    using EmojiLookup = std::function<std::vector<std::string>(const std::vector<std::string>&)>;
    void set_emoji(EmojiLookup lookup) { emoji_ = std::move(lookup); }
    // Common words a reading of two or more kana completes to, which live
    // candidates offer first while they are commoner than the reading as a
    // word, else after the learned ones (JapaneseWords::complete).
    void set_words(const JapaneseWords* words) { words_ = words; }
    // Kaomoji for a reading (japanese/kaomoji.tsv), which live candidates
    // offer after its emoji, all of them; かお and かおもじ list every one.
    void set_kaomoji(const JapaneseWords* kaomoji) { kaomoji_ = kaomoji; }
    // Common chat phrases (japanese/preferred.tsv) complete a reading too,
    // before the words do: いただき to いただきます.
    void set_phrases(const JapaneseWords* phrases) { phrases_ = phrases; }
    // The usual conversions of a whole reading, most wanted first, such as
    // 行きます for いきます, after the user's own choice when they chose
    // otherwise (生きます); none for most readings. Live candidates offer
    // them first, and 変換 converts to the first, as one segment when the
    // converter splits the reading otherwise.
    using Preferred = std::function<std::vector<std::string>(const std::string& reading)>;
    void set_preferred(Preferred preferred) { preferred_ = std::move(preferred); }
    bool empty() const { return reading_.empty() && pending_.empty(); }
    // The kana typed, without romaji still pending.
    const std::string& reading() const { return reading_; }
    // What ↶ brings back for the phrase commit_text() gives: the reading, or
    // for an emoji picked from the live candidates the kana it replaced
    // (ねこ for 🐈).
    std::string reopen_reading() const;
    bool converting() const { return !segments_.empty(); }
    std::vector<std::string> candidates() const;
    int selected() const;
    int active_segment() const { return active_; }
    int segment_count() const { return static_cast<int>(segments_.size()); }

  private:
    struct Segment {
        std::string reading;
        std::vector<std::string> candidates;
        int selected{};
    };
    struct Live {
        // Where each text came from: the whole phrase, a learned word, a
        // candidate of the first segment (its index), or the reading in a script.
        enum class Source { Whole, Prediction, Completion, First, Hiragana, Katakana, Emoji };
        std::string reading; // Converted for these; empty when there are none.
        std::vector<Segment> segments;
        std::vector<std::string> texts;
        std::vector<std::pair<Source, int>> sources;
    };
    Factory factory_;
    std::unique_ptr<JapaneseConverter> converter_;
    std::vector<Segment> segments_;
    std::string reading_, pending_;
    // Where in reading_ typing goes, a byte offset; pending_ waits there.
    std::size_t cursor_{};
    int active_{};
    Live live_;
    // What the converter last converted, unchanged since: live segments can
    // stand for a conversion of it, and resizing continues from it.
    std::string converted_;
    bool resized_{};
    // A preferred phrase 変換 made one segment of, and how many kana ← cut
    // from its end into segments of their own, the phrase's text cut so.
    struct Phrase {
        Segment whole;
        std::string text; // The phrase as chosen when cutting began.
        int cut{};
    };
    std::optional<Phrase> phrase_;
    // Resizes a preferred phrase's segment by its own text; false when its
    // text doesn't end with the kana to cut, for the converter's split.
    bool resize_phrase(int direction);
    bool learning_{};
    EmojiLookup emoji_;
    const JapaneseWords* words_{};
    const JapaneseWords* kaomoji_{};
    const JapaneseWords* phrases_{};
    // The reading came from romaji, whose nn may have meant ん and a な-row
    // kana instead (prepare_live).
    bool romaji_{};
    Preferred preferred_;
    // A learned word picked from the live candidates, now the reading.
    std::optional<std::string> predicted_;
    // An emoji picked from the live candidates, now the reading, and the
    // reading it replaced.
    struct EmojiChoice {
        std::string emoji, reading;
    };
    std::optional<EmojiChoice> emoji_choice_;
    void drain(bool finish);
    // With `preferring`, a common phrase converts to its usual conversion.
    void convert(bool preferring);
    void append(const std::string& text);
    std::string before_cursor() const;
    void erase_before_cursor();
    JapaneseConverter& converter();
    std::vector<Segment> fresh(const std::string& reading);
    std::vector<Segment> imported(std::vector<JapaneseConverter::Segment> segments) const;
    // The reading as conversion would see it, if no romaji letter still waits.
    bool settled(std::string& reading) const;
};
} // namespace framekeyboard

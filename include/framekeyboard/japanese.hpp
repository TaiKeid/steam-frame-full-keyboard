#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
    // Whether the last conversion weighed the whole sentence, as Mozc does:
    // a particle it reads as part of a word then stays so (京都 for きょうと,
    // not 今日と).
    virtual bool weighs_context() const { return false; }
    // Whether a fallback made the last conversion: Anthy, while Mozc
    // couldn't answer. The simulator marks what it offers then.
    virtual bool fell_back() const { return false; }
};
// A converter using the system's Anthy, loaded on first use.
std::unique_ptr<JapaneseConverter> anthy_converter();
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
// The conversion worker and the VR thread both read it: it is read in once,
// and only read after that.
class JapaneseWords {
  public:
    explicit JapaneseWords(std::filesystem::path file = {}) : file_(std::move(file)) {}
    // Reads the list in now, as the conversion worker does ahead of need.
    void preload() const { load(); }
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
    // by complete()'s rule but for an adjective's stem. A reading the list
    // lacks counts as a word of cost `unlisted`, by default none.
    bool completion_leads(const std::string& reading, int cost,
                          int unlisted = std::numeric_limits<int>::max()) const;
    // The reading's lowest cost as a word, or as a word and a particle a
    // little more; the largest int when it is neither.
    int own_cost(const std::string& reading) const;
    // Whether the reading is no word itself but a word and a particle.
    bool word_and_particle(const std::string& reading) const;
    // Up to `most` words of exactly this reading, lowest cost first.
    std::vector<std::string> exact(const std::string& reading, std::size_t most) const;

  private:
    struct Entry {
        std::uint32_t reading{}, word{}; // Offsets into text_.
        std::uint16_t reading_size{}, word_size{}, cost{};
    };
    void load() const;
    void read() const;
    std::filesystem::path file_;
    // Movable, as App assigns its lists; a copy would read the file again.
    std::unique_ptr<std::once_flag> loaded_ = std::make_unique<std::once_flag>();
    mutable std::string text_;
    mutable std::vector<Entry> entries_; // By reading, as the file is.
};
// Pure local preedit state. Nothing reaches an application until explicit commit.
// A copy has the reading, the conversion and the candidates, but not the
// converter: the conversion worker gives its copies its own (use_converter()).
class JapaneseComposer {
  public:
    using Factory = std::function<std::unique_ptr<JapaneseConverter>()>;
    // Without a factory, conversion loads Anthy.
    explicit JapaneseComposer(Factory factory = {});
    ~JapaneseComposer();
    // The keyboard's composer converts nothing itself: a step that would
    // call the converter throws Deferred, and the keyboard takes it to the
    // conversion worker, running it on a copy of the composer as it was
    // before the step (adopt()).
    struct Deferred : std::runtime_error {
        Deferred() : std::runtime_error("Conversion runs on the conversion worker") {}
    };
    void defer_conversion() { deferred_ = true; }
    // The converter this copy converts with, which the worker owns; with
    // none, the copy converts nothing, as the keyboard's composer.
    void use_converter(JapaneseConverter* converter) {
        converter_.borrowed = converter;
        deferred_ = converter == nullptr;
    }
    // What the converter last converted, as fresh() and resize() leave it,
    // which resizing and learning continue from. The worker's is the true
    // one: it gives it to every copy it converts and takes it back with
    // the answer, and the keyboard's composer keeps a copy.
    struct Record;
    Record record() const;
    void set_record(const Record& record);
    // Before learn(), for a converter that converted another reading since:
    // makes its last conversion this phrase's again, converting it again.
    // Nothing for a phrase learn() doesn't learn from the conversion; false
    // for one resized, which converting again can't restore.
    bool reconvert();
    // Takes the answer of a copy the worker ran a step on, keeping this
    // composer's converter and lists. adopt() takes all of it, for a step
    // while this one stayed as it was (edits()); adopt_live() its live
    // candidates, unless they were cleared since, and its record.
    void adopt(JapaneseComposer done);
    void adopt_live(JapaneseComposer done);
    // Counts the steps that change the reading or conversion, and the
    // times the live candidates were cleared.
    std::uint64_t edits() const { return edits_; }
    std::uint64_t clears() const { return cleared_; }
    // The reading live candidates would be for; false while a romaji letter
    // still waits that could change it.
    bool live_reading(std::string& reading) const { return settled(reading); }
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
    // Whether the last conversion, for the live candidates or 変換, came
    // from the converter's fallback (JapaneseConverter::fell_back()).
    bool fell_back() const {
        const auto* converter = converter_.get();
        return converter ? converter->fell_back() : fell_back_;
    }
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
    // The words of the converted phrase commit_text() gives that have a usual
    // form (set_preferred), each with the form it was committed in, without
    // its particle: いく and 生く for 生くね. Remembered as the user's choice,
    // a form picked so beats the usual one in later sentences too.
    std::vector<std::pair<std::string, std::string>> word_choices() const;
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
    // The form last picked for a listed word, by its reading as listed (いく,
    // があう), or empty. Inside a sentence it leads the word, as on a phone,
    // while a reading typed on its own takes set_preferred()'s order.
    using WordPick = std::function<std::string(const std::string& reading)>;
    void set_word_pick(WordPick pick) { word_pick_ = std::move(pick); }
    // Whether segments take the usual forms of their words (on by default);
    // a converter that weighs the whole sentence, as Mozc, may do better
    // without.
    void set_word_forms(bool on) { word_forms_ = on; }
    bool empty() const { return reading_.empty() && pending_.empty(); }
    // The kana typed, without romaji still pending.
    const std::string& reading() const { return reading_; }
    // What ↶ brings back for the phrase commit_text() gives: the typed reading,
    // also for a completion or emoji picked from the live candidates
    // (おもしろ for 面白い, ねこ for 🐈).
    std::string reopen_reading() const;
    // The reading typed for the phrase commit_text() gives, also where a
    // word picked from the live candidates replaced it (すぐいく for すぐ行く).
    std::string typed_reading() const;
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
    // The converter: made by the factory on first need, or the worker's
    // (use_converter()). A copy of the composer starts without either, so
    // copies never share one; a move takes it along.
    struct ConverterSlot {
        std::unique_ptr<JapaneseConverter> owned;
        JapaneseConverter* borrowed{};
        ConverterSlot() = default;
        ConverterSlot(const ConverterSlot&) {}
        ConverterSlot& operator=(const ConverterSlot&) {
            owned.reset();
            borrowed = nullptr;
            return *this;
        }
        ConverterSlot(ConverterSlot&&) noexcept = default;
        ConverterSlot& operator=(ConverterSlot&&) noexcept = default;
        JapaneseConverter* get() const { return borrowed ? borrowed : owned.get(); }
    };
    // adopt() keeps the converter and factory, deferred_, learning_ and
    // what the set_*() calls gave, the composer's own, and takes the rest
    // from the copy: a member of that kind added must be kept there too.
    Factory factory_;
    ConverterSlot converter_;
    bool deferred_{};
    // The keyboard's composer's copies of the worker's (record()).
    bool fell_back_{};
    std::uint64_t conversion_{};
    std::uint64_t edits_{};
    // Counts the times live_ was cleared, so that an answer made before
    // cannot bring its candidates back (adopt_live()).
    std::uint64_t cleared_{};
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
    // Where a segment stands in its phrase: the reading of the one before it,
    // empty for the first, and whether it ends the phrase.
    struct Place {
        std::string before;
        bool last{true};
        bool context{}; // The converter weighed the sentence (weighs_context()).
    };
    // A word's usual forms by the phrase list, the one to use first, and the
    // reading the list has them under: after a segment that ends in a case
    // particle, the list's context entry for it where it has one (〜があう,
    // 〜が合う); at the phrase's end also for one that begins with it.
    std::optional<std::pair<std::string, std::vector<std::string>>> word_forms(const std::string& word,
                                                                               const Place& place) const;
    // Whether Anthy's `text` writes `reading` as a common word, by the word
    // list, and kana after it: 車で for くるまで.
    bool common_word(const std::string& reading, const std::string& text) const;
    // A segment reading as a listed word and a particle after it, where the
    // word's form applies: not where Anthy's `text` reads the particle as
    // part of a common word, nor for a hiragana form before a particle a
    // noun takes.
    std::optional<std::pair<std::string, std::string>>
    particle_split(const std::string& reading, const std::string& text, const Place& place) const;
    // A segment reading's usual forms, also before a particle; none without.
    std::vector<std::string> usual_forms(const std::string& reading, const std::string& text,
                                         const Place& place) const;
    // The forms listed under `key` that begin with `prefix`, without it, the
    // user's latest pick first (set_word_pick()).
    std::vector<std::string> listed_forms(const std::string& key, const std::string& prefix) const;
    // A listed word at the end of a segment reading: its key in the list,
    // the kana Anthy's text ends with for it, and the forms for those. After
    // a case particle, at the phrase's end, the list's context entry for it
    // (仕事であった, 〜であった, であった, で会った), else after a tail particle
    // the word's own forms; not in a word Anthy kept whole.
    struct Tail {
        std::string key, kana;
        std::vector<std::string> forms;
    };
    std::optional<Tail> tail_word(const std::string& reading, const std::string& text,
                                  const Place& place) const;
    // `text` with a kana word Anthy left at its end, after に, へ, と or
    // から, in its usual form; empty without one.
    std::string usual_tail(const std::string& reading, const std::string& text,
                           const Place& place) const;
    // The reading of the word with a usual form a segment's reading holds, as
    // usual_forms() and usual_tail() find it, and the form `text` writes it
    // in, a context entry's mark and particle first; none when `text` writes
    // it otherwise than they would.
    std::optional<std::pair<std::string, std::string>>
    word_choice(const std::string& reading, const std::string& text, const Place& place) const;
    // Resizes a preferred phrase's segment by its own text; false when its
    // text doesn't end with the kana to cut, for the converter's split.
    bool resize_phrase(int direction);
    // A ー that stretches a word, as chat writes it: on its end (いいよー), or
    // before a final い or う (ばいばーい), `final` then.
    struct Stretch {
        std::string marks, final;
        bool empty() const { return marks.empty(); }
        // `text` stretched so; empty when it doesn't end with the final kana.
        std::string apply(const std::string& text) const;
    };
    // Whether each of `segments`, converted from `typed` with romaji's n
    // carried over, that holds a kana the carried n changed is a word.
    bool carried_words(const std::vector<Segment>& segments, const std::string& typed,
                       const std::string& converted) const;
    // Puts a stretching ー back on the segments converted without it.
    static void stretch_segments(std::vector<Segment>& segments, Stretch stretch);
    // The usual conversions of `reading`, else of the reading as converted,
    // the n carried over, a stretching ー kept on.
    std::vector<std::string> usual_phrases(const std::string& reading) const;
    bool learning_{};
    EmojiLookup emoji_;
    const JapaneseWords* words_{};
    const JapaneseWords* kaomoji_{};
    const JapaneseWords* phrases_{};
    // The reading came from romaji, whose nn may have meant ん and a な-row
    // kana instead (prepare_live).
    bool romaji_{};
    // The reading as Anthy converted it for converted_, where it differs:
    // romaji's n carried over, a stretching ー left off (fresh()); that ー,
    // which the conversion keeps; and on a tie between romaji's readings,
    // the other's whole phrase, which the live candidates offer second.
    std::string converted_as_;
    // Numbers Mozc reads as kanji numerals, in digits where it offers them
    // (四年も as 4年も, 二百六十|年 as 260|年), in place.
    static void numeral_digits(std::vector<Segment>& segments);
    // 階 where Mozc reads 回 for a floor (6階のエレベーター), in place.
    static void floors(std::vector<Segment>& segments);
    // Verbs the object before them decides (雪降って, 写真を撮って), in place.
    static void object_verbs(std::vector<Segment>& segments);
    // Words chat writes in kana (ください, なし, できる), in place.
    static void chat_kana(std::vector<Segment>& segments);
    Stretch stretch_;
    std::string alternative_;
    Preferred preferred_;
    WordPick word_pick_;
    bool word_forms_ = true;
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
struct JapaneseComposer::Record {
    std::string converted, converted_as, alternative;
    Stretch stretch;
    bool resized{}, fell_back{};
    // The worker's count of the conversions it made, its last one's.
    std::uint64_t conversion{};
};
} // namespace framekeyboard

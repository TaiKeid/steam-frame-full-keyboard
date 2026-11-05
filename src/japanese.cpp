#include "framekeyboard/japanese.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <glib.h>
#include <map>
#include <mutex>
#include <pwd.h>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unistd.h>

namespace framekeyboard {
namespace {
std::string normalized(const std::string& text) {
    auto* value = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_DEFAULT_COMPOSE);
    std::string result = value ? value : text;
    g_free(value);
    return result;
}
const std::map<std::string, std::string>& roman_table() {
    static const auto table = [] {
        std::map<std::string, std::string> result;
        // Hepburn and common Japanese IME spellings, including small kana.
        std::istringstream entries(
            "a あ i い u う e え o お "
            "ka か ki き ku く ke け ko こ ca か ci し cu く ce せ co こ "
            "ga が gi ぎ gu ぐ ge げ go ご "
            "sa さ si し shi し su す se せ so そ za ざ zi じ ji じ zu ず ze ぜ zo ぞ "
            "ta た ti ち chi ち tu つ tsu つ te て to と da だ di ぢ du づ de で do ど "
            "na な ni に nu ぬ ne ね no の ha は hi ひ hu ふ fu ふ he へ ho ほ "
            "ba ば bi び bu ぶ be べ bo ぼ pa ぱ pi ぴ pu ぷ pe ぺ po ぽ "
            "ma ま mi み mu む me め mo も ya や yu ゆ yo よ ra ら ri り ru る re れ ro ろ "
            "wa わ wi うぃ wu う we うぇ wo を va ゔぁ vi ゔぃ vu ゔ ve ゔぇ vo ゔぉ "
            "sha しゃ shu しゅ sho しょ she しぇ cha ちゃ chu ちゅ cho ちょ che ちぇ "
            "ja じゃ ju じゅ jo じょ je じぇ fa ふぁ fi ふぃ fe ふぇ fo ふぉ "
            "fya ふゃ fyu ふゅ fyo ふょ tsa つぁ tsi つぃ tse つぇ tso つぉ "
            "tha てゃ thi てぃ thu てゅ the てぇ tho てょ dha でゃ dhi でぃ dhu でゅ dhe でぇ dho でょ "
            "twu とぅ dwu どぅ kwa くぁ gwa ぐぁ ye いぇ tcha っちゃ tchu っちゅ tcho っちょ "
            "xa ぁ xi ぃ xu ぅ xe ぇ xo ぉ xya ゃ xyu ゅ xyo ょ xtu っ xtsu っ xwa ゎ xka ヵ xke ヶ "
            "la ぁ li ぃ lu ぅ le ぇ lo ぉ lya ゃ lyu ゅ lyo ょ ltu っ ltsu っ lwa ゎ lka ヵ lke ヶ "
            "- ー , 、 . 。 [ 「 ] 」 / ・ ? ？ ! ！ \\ ＼ | ｜ xn ん");
        std::string from, to;
        while (entries >> from >> to) {
            result[from] = to;
        }
        const std::map<std::string, std::string> palatal = {
            {"ky", "き"}, {"gy", "ぎ"}, {"sy", "し"}, {"zy", "じ"}, {"jy", "じ"},
            {"ty", "ち"}, {"cy", "ち"}, {"dy", "ぢ"}, {"ny", "に"}, {"hy", "ひ"},
            {"by", "び"}, {"py", "ぴ"}, {"my", "み"}, {"ry", "り"}};
        for (const auto& [prefix, kana] : palatal) {
            result[prefix + "a"] = kana + "ゃ";
            result[prefix + "u"] = kana + "ゅ";
            result[prefix + "o"] = kana + "ょ";
            result[prefix + "e"] = kana + "ぇ";
        }
        return result;
    }();
    return table;
}
// Load the existing system Anthy only on the first conversion. Its public C ABI
// keeps ordinary keyboard startup independent of the optional Japanese engine.
struct AnthyStat {
    int segments;
};
struct AnthySegmentStat {
    int candidates, length;
};
struct AnthyPredictionStat {
    int predictions;
};
// Full Keyboard's personality: Anthy keeps what it learns here under this
// name, apart from the stock keyboard's "default", which it imports once
// (docs/japanese.md, Learning).
constexpr const char* learning_id = "framekeyboard";
constexpr const char* stock_id = "default";
constexpr int hiragana_candidate = -3, katakana_candidate = -2; // NTH_*_CANDIDATE
std::string in_script(const std::string& reading, bool katakana);
class AnthyConverter;
// Anthy, loaded once and shared by every converter. `calls` is held around
// every Anthy call, so a restart on a worker thread never overlaps one;
// `settings` guards the learning, which the VR thread reads without waiting
// for a restart.
struct Anthy {
    struct Api {
        void* handle{};
        int (*init)(){};
        void (*quit)(){};
        void* (*create)(){};
        void (*release)(void*){};
        int (*encoding)(void*, int){};
        int (*set)(void*, const char*){};
        int (*stat)(void*, AnthyStat*){};
        int (*segment_stat)(void*, int, AnthySegmentStat*){};
        int (*get)(void*, int, int, char*, int){};
        void (*resize)(void*, int, int){};
        // Learning's: in every Anthy release, but checked like the rest.
        void (*conf_override)(const char*, const char*){};
        int (*personality)(const char*){};
        int (*commit)(void*, int, int){};
        int (*predict)(void*, const char*){};
        int (*prediction_stat)(void*, AnthyPredictionStat*){};
        int (*prediction)(void*, int, char*, int){};
        int (*commit_prediction)(void*, int){};
        bool learns() const {
            return conf_override && personality && commit && predict && prediction_stat && prediction &&
                   commit_prediction;
        }
    };
    std::mutex calls;
    std::optional<Api> api;
    std::set<AnthyConverter*> converters;
    std::mutex settings;
    AnthyLearning learning;
    std::string dictionary;               // Empty: the installed one.
    std::optional<AnthyLearning> started; // What the running Anthy learns with.
    bool configured{};
    ~Anthy() {
        if (api) {
            api->quit();
            dlclose(api->handle);
        }
    }
};
Anthy& anthy() {
    // Anthy initialization is process-wide. Share its lifetime across previews/tests.
    static Anthy value;
    return value;
}
// Anthy's home: the configured one, or the user's own. Anthy itself asks the
// password database; HOME comes first here, as for every other program, so a
// run with a home of its own (tests, for one) keeps Anthy's files there.
std::filesystem::path learning_home(const AnthyLearning& learning) {
    if (!learning.home.empty()) {
        return learning.home;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return home;
    }
    const auto* user = getpwuid(getuid());
    return user && user->pw_dir ? user->pw_dir : "";
}
// Initializes a loaded Anthy with the configured learning; under `calls`.
void start(Anthy& state) {
    auto& api = *state.api;
    const auto [learning, dictionary] = [&] {
        std::lock_guard lock(state.settings);
        return std::pair{state.learning, state.dictionary};
    }();
    // Anthy forgets its settings when it quits, so each start gives them again.
    if (!dictionary.empty() && api.conf_override) {
        api.conf_override("DIC_FILE", dictionary.c_str());
    }
    const auto home = learning_home(learning);
    if (api.learns() && !home.empty()) {
        api.conf_override("HOME", home.c_str());
    }
    if (api.init() != 0) {
        throw std::runtime_error("Cannot initialize the Anthy dictionary");
    }
    // Off, an anonymous personality keeps no record and reads none, not even
    // the stock keyboard's.
    if (api.personality) {
        api.personality(learning.on && api.learns() ? learning_id : "");
    }
    std::lock_guard lock(state.settings);
    state.started = learning;
}
// The loaded Anthy, loading it on first use; under `calls`.
Anthy::Api& loaded() {
    auto& state = anthy();
    if (state.api) {
        return *state.api;
    }
    Anthy::Api api;
    api.handle = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!api.handle) {
        throw std::runtime_error("Japanese conversion needs the Anthy library");
    }
    auto symbol = [&](auto& fn, const char* name, bool required = true) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(api.handle, name));
        if (!fn && required) {
            throw std::runtime_error("Installed Anthy has an incompatible API");
        }
    };
    try {
        symbol(api.init, "anthy_init");
        symbol(api.quit, "anthy_quit");
        symbol(api.create, "anthy_create_context");
        symbol(api.release, "anthy_release_context");
        symbol(api.encoding, "anthy_context_set_encoding");
        symbol(api.set, "anthy_set_string");
        symbol(api.stat, "anthy_get_stat");
        symbol(api.segment_stat, "anthy_get_segment_stat");
        symbol(api.get, "anthy_get_segment");
        symbol(api.resize, "anthy_resize_segment");
        symbol(api.conf_override, "anthy_conf_override", false);
        symbol(api.personality, "anthy_set_personality", false);
        symbol(api.commit, "anthy_commit_segment", false);
        symbol(api.predict, "anthy_set_prediction_string", false);
        symbol(api.prediction_stat, "anthy_get_prediction_stat", false);
        symbol(api.prediction, "anthy_get_prediction", false);
        symbol(api.commit_prediction, "anthy_commit_prediction", false);
        state.api = api;
        start(state);
    } catch (...) {
        state.api.reset();
        dlclose(api.handle);
        throw;
    }
    return *state.api;
}
// The VR thread never waits for a restart on the worker: a conversion asked
// for meanwhile says so, and learning lets the phrase go.
std::unique_lock<std::mutex> unless_restarting() {
    std::unique_lock lock(anthy().calls, std::try_to_lock);
    if (!lock) {
        throw std::runtime_error("Japanese conversion is restarting. Try again in a moment.");
    }
    return lock;
}
class AnthyConverter final : public JapaneseConverter {
  public:
    AnthyConverter() {
        const auto lock = unless_restarting();
        loaded();
        context();
        anthy().converters.insert(this);
    }
    ~AnthyConverter() override {
        std::lock_guard lock(anthy().calls);
        release();
        anthy().converters.erase(this);
    }
    std::vector<Segment> convert(const std::string& reading) override {
        const auto lock = unless_restarting();
        if (loaded().set(context(), reading.c_str()) != 0) {
            throw std::runtime_error("Japanese conversion failed");
        }
        return segments();
    }
    std::vector<Segment> resize(int index, int delta) override {
        const auto lock = unless_restarting();
        auto& api = loaded();
        AnthyStat stat{};
        // A restart took the last conversion with it: nothing to resize.
        if (!context_ || api.stat(context_, &stat) != 0 || index < 0 || index >= stat.segments) {
            return {};
        }
        // Anthy leaves the segments as they were when the change is impossible.
        api.resize(context_, index, delta);
        return segments();
    }
    void learn(const std::vector<std::string>& chosen) override {
        std::unique_lock lock(anthy().calls, std::try_to_lock);
        if (!lock || !learning() || !context_) {
            return;
        }
        auto& api = *anthy().api;
        AnthyStat stat{};
        if (api.stat(context_, &stat) != 0 || stat.segments != static_cast<int>(chosen.size())) {
            return;
        }
        // Anthy learns once every segment is committed. A choice it did not
        // offer, such as a symbol typed, leaves the phrase unlearned.
        std::vector<int> picks;
        for (int s = 0; s < stat.segments; ++s) {
            const auto pick = index_of(s, chosen[static_cast<std::size_t>(s)]);
            if (!pick) {
                return;
            }
            picks.push_back(*pick);
        }
        for (int s = 0; s < stat.segments; ++s) {
            api.commit(context_, s, picks[static_cast<std::size_t>(s)]);
        }
    }
    std::vector<std::string> predict(const std::string& reading) override {
        std::unique_lock lock(anthy().calls, std::try_to_lock);
        std::vector<std::string> result;
        if (!lock || !learning()) {
            return result;
        }
        auto& api = *anthy().api;
        AnthyPredictionStat stat{};
        if (api.predict(predicting(), reading.c_str()) != 0 ||
            api.prediction_stat(predicting(), &stat) != 0) {
            return result;
        }
        for (int i = 0; i < std::min(stat.predictions, max_predictions); ++i) {
            char buffer[1024]{};
            if (api.prediction(predicting(), i, buffer, sizeof(buffer)) > 0 &&
                g_utf8_validate(buffer, -1, nullptr)) {
                result.emplace_back(buffer);
            }
        }
        return result;
    }
    void learn_prediction(const std::string& reading, const std::string& text) override {
        std::unique_lock lock(anthy().calls, std::try_to_lock);
        if (!lock || !learning()) {
            return;
        }
        auto& api = *anthy().api;
        AnthyPredictionStat stat{};
        if (api.predict(predicting(), reading.c_str()) != 0 ||
            api.prediction_stat(predicting(), &stat) != 0) {
            return;
        }
        for (int i = 0; i < stat.predictions; ++i) {
            char buffer[1024]{};
            if (api.prediction(predicting(), i, buffer, sizeof(buffer)) > 0 && text == buffer) {
                api.commit_prediction(predicting(), i);
                return;
            }
        }
    }
    // Before Anthy restarts; the next call makes new contexts. Under `calls`.
    void release() {
        for (auto* context : {&context_, &prediction_context_}) {
            if (*context) {
                anthy().api->release(*context);
                *context = nullptr;
            }
        }
    }

  private:
    static constexpr int max_predictions = 8;
    static bool learning() {
        auto& state = anthy();
        std::lock_guard lock(state.settings);
        return state.api && state.api->learns() && state.started && state.started->on;
    }
    static void* made() {
        auto& api = loaded();
        void* context = api.create();
        if (!context) {
            throw std::runtime_error("Cannot create Japanese conversion context");
        }
        api.encoding(context, 2); // ANTHY_UTF8_ENCODING
        return context;
    }
    void* context() {
        if (!context_) {
            context_ = made();
        }
        return context_;
    }
    // Predictions have a context of their own: asking for them starts a new
    // dictionary session, which a conversion still to be committed needs.
    void* predicting() {
        if (!prediction_context_) {
            prediction_context_ = made();
        }
        return prediction_context_;
    }
    // Anthy's segment text at `candidate`, or its reading at -1
    // (NTH_UNCONVERTED_CANDIDATE).
    std::string text(int segment, int candidate) {
        char buffer[1024]{};
        if (anthy().api->get(context_, segment, candidate, buffer, sizeof(buffer)) < 0 ||
            !g_utf8_validate(buffer, -1, nullptr)) {
            throw std::runtime_error("Invalid Japanese conversion candidate");
        }
        return buffer;
    }
    // Where Anthy offers `chosen` for a segment of its last conversion, as it
    // numbers candidates: its own, or the reading in either script.
    std::optional<int> index_of(int segment, const std::string& chosen) {
        AnthySegmentStat info{};
        if (anthy().api->segment_stat(context_, segment, &info) != 0) {
            return {};
        }
        try {
            for (int c = 0; c < info.candidates; ++c) {
                if (text(segment, c) == chosen) {
                    return c;
                }
            }
            const auto reading = text(segment, -1);
            if (chosen == in_script(reading, false)) {
                return hiragana_candidate;
            }
            if (chosen == in_script(reading, true)) {
                return katakana_candidate;
            }
        } catch (const std::exception&) {
        }
        return {};
    }
    std::vector<Segment> segments() {
        auto& api = *anthy().api;
        AnthyStat stat{};
        if (api.stat(context_, &stat) != 0 || stat.segments <= 0 || stat.segments > 32) {
            throw std::runtime_error("Japanese conversion failed");
        }
        std::vector<Segment> result;
        for (int s = 0; s < stat.segments; ++s) {
            AnthySegmentStat info{};
            if (api.segment_stat(context_, s, &info) != 0 || info.candidates <= 0) {
                throw std::runtime_error("No conversion candidates");
            }
            Segment segment{text(s, -1), {}};
            for (int c = 0; c < std::min(info.candidates, 128); ++c) {
                segment.candidates.push_back(text(s, c));
            }
            result.push_back(std::move(segment));
        }
        return result;
    }
    void* context_{};
    void* prediction_context_{};
};
// The reading in one script; other characters stay.
std::string in_script(const std::string& reading, bool katakana) {
    std::string result;
    for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
        auto c = g_utf8_get_char(p);
        if (katakana && c >= 0x3041 && c <= 0x3096) {
            c += 0x60;
        }
        if (!katakana && c >= 0x30a1 && c <= 0x30f6) {
            c -= 0x60;
        }
        char buffer[8]{};
        const auto n = g_unichar_to_utf8(c, buffer);
        result.append(buffer, static_cast<std::size_t>(n));
    }
    return result;
}
// Whether the text has a kanji: a word read from the dictionary, not kana.
bool has_kanji(const std::string& text) {
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        if (g_unichar_get_script(g_utf8_get_char(p)) == G_UNICODE_SCRIPT_HAN) {
            return true;
        }
    }
    return false;
}
// Punctuation and symbols alone, such as ！, 」 or …, with no kana.
bool symbols_only(const std::string& reading) {
    for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
        if (!g_unichar_ispunct(g_utf8_get_char(p))) {
            return false;
        }
    }
    return !reading.empty();
}
// Numbers spelled in kana before a counter, as digits: さんじゅっぷん as
// 30ぷん, いちまんえん as 1まんえん, which Mozc converts to 30分 and 1万円, as
// chat writes them (Mozc reads the kana as words: 三拾分, 糸満円). One digit
// alone counts only where it and its counter are no common word (いっぱい,
// いちばん, にほん stay), and に, よ, く and し only at the reading's start,
// as に is mostly a particle. Days after a month count too (じゅうがつはつか).
namespace numbers {
struct Morpheme {
    std::string_view kana;
    int value;       // A digit, or a unit: 10, 100, 1000, 10000, 10^8.
    bool geminate{}; // いっ, じゅっ: a counter must follow at once.
    int after{};     // A unit's sound only after this digit (さんびゃく, はっぴゃく).
};
// Longest first where one begins another (しち before し).
constexpr Morpheme morphemes[] = {
    {"きゅう", 9},
    {"いち", 1},
    {"いっ", 1, true},
    {"さん", 3},
    {"よん", 4},
    {"ろく", 6},
    {"ろっ", 6, true},
    {"なな", 7},
    {"しち", 7},
    {"はち", 8},
    {"はっ", 8, true},
    {"に", 2},
    {"ご", 5},
    {"よ", 4},
    {"く", 9},
    {"し", 4},
    {"じゅっ", 10, true},
    {"じっ", 10, true},
    {"じゅう", 10},
    {"ひゃっ", 100, true},
    {"ひゃく", 100},
    {"びゃく", 100, false, 3},
    {"ぴゃく", 100, false, 8},
    {"ぴゃく", 100, false, 6},
    {"せん", 1000},
    {"ぜん", 1000, false, 3},
    {"まん", 10000},
    {"おく", 100000000},
};
// Counters, longest first where one begins another. No げん (三軒, 宣言) or
// bare しゅう (ご祝儀, 実習).
constexpr std::string_view counters[] = {
    "ぱーせんと", "じかんめ",   "じかん",   "じげんめ",     "じはん",     "じ",       "にんまえ",
    "にちかん",   "にち",       "にん",     "ねんせい",     "ねんだい",   "ねんかん", "ねんまえ",
    "ねん",       "かげつかん", "かげつ",   "しゅうかん",   "しゅうねん", "ぷんかん", "ふんかん",
    "ぷん",       "ふん",       "びょう",   "ばんめ",       "ばんせん",   "ばん",     "かいめ",
    "かい",       "がつ",       "えん",     "まい",         "ほん",       "ぼん",     "ぽん",
    "ひき",       "びき",       "ぴき",     "さつ",         "だい",       "さい",     "てん",
    "ぺーじ",     "もん",       "ばい",     "きろめーとる", "きろぐらむ", "きろ",     "めーとる",
    "せんち",     "ぐらむ",     "こ",       "ど",           "ごうしつ",   "がい",     "りっとる",
    "れんしょう", "れんぱい",   "れんぱつ", "ぴーす"};
// A counter followed so starts another word: さんほんとう is さん + 本当,
// きゅうじつ 休日.
constexpr std::pair<std::string_view, std::string_view> joined[] = {
    {"ほん", "と"},   {"ほん", "や"}, {"だい", "がく"}, {"てん", "き"},   {"まい", "にち"},
    {"かい", "しゃ"}, {"かい", "ぎ"}, {"かい", "わ"},   {"えん", "りょ"}, {"こ", "と"},
    {"じ", "つ"},     {"にん", "き"}, {"じ", "き"},     {"ぼん", "じめ"}, {"がつ", "びょう"},
    {"ぽん", "ぎ"},   {"じ", "こ"},   {"じ", "ぶん"},   {"じ", "しん"},   {"じ", "む"},
    {"じ", "けん"},   {"じ", "たく"}, {"じ", "び"},     {"じ", "のぼ"}};
// A digit alone with its counter that is a word, kept as Mozc converts it.
constexpr std::string_view words[] = {
    "いっぱい",   "いちばん",   "いちにち", "いっさい", "いちじ",     "にじ",       "くじ",
    "ごじ",       "にほん",     "ごかい",   "ごえん",   "じゅうぶん", "じゅうだい", "じゅうてん",
    "せんしゅう", "じゅうにん", "ごにん",   "いっこ",   "いっかい",   "きゅうえん", "いちがい"};
// What may come before a digit alone, mid-reading: a particle's last kana,
// a clause's て, or punctuation.
constexpr std::string_view boundaries[] = {"は", "が", "を", "で", "と", "も",   "の",
                                           "へ", "に", "て", "、", "。", "から", "まで"};
// Words a number follows, as a particle: もうよねん, ちょうどじゅうねん,
// やくにひゃく, ぜんじゅうにわ; に leads a number after them too.
constexpr std::string_view number_words[] = {"もう", "あと", "ちょうど", "やく", "およそ", "ぜん"};
// Counters a digit alone never takes, too often the start of a word (しこみ,
// ろっこつ, いちてんもの): after longer numbers only (50個, 100点).
constexpr std::string_view longer_only[] = {"こ", "てん", "もん", "ど"};
// The counters し, よ and く take as digits: しがつ, よじ, くじかん.
constexpr std::pair<std::string_view, std::string_view> restricted_counters[] = {
    {"し", "がつ"},   {"よ", "じ"}, {"よ", "にん"},   {"よ", "ねん"}, {"よ", "じかん"},
    {"よ", "じはん"}, {"く", "じ"}, {"く", "じかん"}, {"く", "がつ"}, {"く", "じはん"}};
// に within a reading, after these, before these counters, is a digit:
// ぴざをにまい 2枚, ひとばんでにかい 2回; not after に, の or へ, where it is
// the particle again (こんびににかいもの), nor と or て, which end words too
// (ひとに, ねっとに). At the reading's start the same counters only.
constexpr std::string_view two_after[] = {"を", "は", "が", "も", "で", "、"};
constexpr std::string_view two_counters[] = {"まい", "かいめ", "かい",   "ひき",      "びき",
                                             "さつ", "だい",   "ふん",   "ぷん",      "ばんめ",
                                             "ばん", "ねん",   "かげつ", "しゅうかん"};
// After a time of day, a digit before じ is the hour, even one that alone is a
// word or a particle: よるくじ 夜9時, ごごにじ 午後2時.
constexpr std::string_view times_of_day[] = {
    "よる",   "あさ", "ごぜん", "ごご",     "ひる",     "ゆうがた", "よなか",
    "しんや", "けさ", "こんや", "こんばん", "まいばん", "まいあさ", "いま",
    "あした", "あす", "きょう", "きのう",   "ようび"}; // 火曜日の4時.
constexpr std::pair<std::string_view, int> days[] = {
    {"ついたち", 1}, {"ふつか", 2}, {"みっか", 3},   {"よっか", 4},  {"いつか", 5}, {"むいか", 6},
    {"なのか", 7},   {"ようか", 8}, {"ここのか", 9}, {"とおか", 10}, {"はつか", 20}};
constexpr std::string_view particles[] = {"は", "が",     "も",     "を",   "の",       "で",  "に",
                                          "と", "くらい", "ぐらい", "ほど", "いじょう", "いか"};
bool any_prefix(std::string_view text, std::span<const std::string_view> list) {
    return std::any_of(list.begin(), list.end(), [&](auto item) { return text.starts_with(item); });
}
// A counter at the start of `rest` that ends where its mora does.
std::string_view counter_at(std::string_view rest) {
    constexpr std::string_view extending[] = {"ゃ", "ゅ", "ょ", "ぁ", "ぃ", "ぅ",
                                              "ぇ", "ぉ", "っ", "ー", "う", "ん"};
    for (const auto counter : counters) {
        if (!rest.starts_with(counter)) {
            continue;
        }
        const auto after = rest.substr(counter.size());
        const bool word = std::any_of(std::begin(joined), std::end(joined), [&](const auto& pair) {
            return pair.first == counter && after.starts_with(pair.second);
        });
        if (!any_prefix(after, extending) && !word) {
            return counter;
        }
    }
    return {};
}
struct Number {
    std::size_t end{};
    long long value{};
    int parts{};
    // tens: に, よ, く or し first, then じゅう; hundreds: に, then ひゃく or せん.
    bool geminate{}, restricted{}, tens{}, hundreds{};
    std::string_view first, last; // The first and last morphemes.
};
// The numbers spelled from `at`, shortest first: groups of digits times せん,
// ひゃく and じゅう, then まん or おく over the group before them. A shorter
// one may be the number where the longer has no counter after it:
// さんじゅうにん is 30 + にん, not 32 + ん.
std::vector<Number> parses(std::string_view text, std::size_t at) {
    std::vector<Number> found_numbers;
    Number best;
    long long total = 0, group = 0;
    int digit = 0, parts = 0, previous = 0;
    bool restricted = false, tens = false, hundreds = false, doubled = false;
    std::string_view first;
    for (std::size_t i = at; i < text.size();) {
        const auto found = std::find_if(std::begin(morphemes), std::end(morphemes), [&](const auto& m) {
            return text.substr(i).starts_with(m.kana) && (!m.after || m.after == previous);
        });
        // A doubled digit is followed by ぴゃく or せん, or by a counter.
        if (found == std::end(morphemes) || (doubled && found->value != 100 && found->value != 1000)) {
            break;
        }
        doubled = found->geminate && found->value < 10;
        if (found->value < 10) {
            if (digit) {
                break; // Two digits in a row are no number.
            }
            digit = found->value;
            if (parts == 0) {
                first = found->kana;
                restricted = found->kana == "に" || found->kana == "よ" || found->kana == "く" ||
                             found->kana == "し";
            }
        } else if (found->value >= 10000) {
            const auto unit = group + digit;
            if (!unit) {
                break; // まん and おく take a number before them.
            }
            total += unit * found->value;
            group = digit = 0;
            if (parts == 1) {
                hundreds = first == "に"; // にまん after a particle.
            }
        } else {
            if (parts == 1) {
                tens = restricted && found->value == 10;
                hundreds = first == "に" && found->value > 10;
            }
            group += (digit ? digit : 1) * found->value;
            digit = 0;
        }
        previous = found->value < 10 ? found->value : 0;
        i += found->kana.size();
        ++parts;
        if (parts == 1 && found->value >= 10) {
            first = found->kana;
        }
        best = {
            i,          total + group + digit, parts, found->geminate, restricted, tens, hundreds, first,
            found->kana};
        found_numbers.push_back(best);
        if (found->geminate && found->value >= 10) {
            break; // じゅっ, ひゃっ: a counter follows.
        }
    }
    return found_numbers;
}
// The longest number spelled from `at`.
Number parse(std::string_view text, std::size_t at) {
    auto found = parses(text, at);
    return found.empty() ? Number{} : found.back();
}
std::string written(long long value) {
    std::string result;
    if (value >= 100000000) {
        result += std::to_string(value / 100000000) + "おく";
        value %= 100000000;
    }
    if (value >= 10000) {
        result += std::to_string(value / 10000) + "まん";
        value %= 10000;
    }
    if (value) {
        result += std::to_string(value);
    }
    return result;
}
// Whether the number at `at` is one, as chat writes it in digits.
bool counts(std::string_view text, std::size_t at, const Number& number, std::string_view counter) {
    const auto rest = text.substr(number.end);
    const bool start = at == 0;
    const auto preceding = text.substr(0, at);
    const auto after_word = [&] {
        return std::any_of(std::begin(number_words), std::end(number_words),
                           [&](auto word) { return preceding.ends_with(word); });
    };
    const auto after_two = [&] { // The particle を, は, が, も, で or 、 before.
        const char* last = g_utf8_prev_char(text.data() + at);
        return any_prefix(text.substr(static_cast<std::size_t>(last - text.data())), two_after) ||
               after_word();
    };
    // に, よ, く or し lead a number at the reading's start, or within it
    // twenty and the like (あとにじゅっぷん), and に two hundred or thousand
    // after a particle (ことしはにせん…; とこにひゃくえん is とこに 100円).
    const bool led = start || !number.restricted || number.tens || (number.hundreds && after_two());
    if (number.first == "し") {
        return number.parts == 1 && counter == "がつ" && start; // しじゅう idioms stay.
    }
    const bool boundary = start || after_word() ||
                          std::any_of(std::begin(boundaries), std::end(boundaries),
                                      [&](auto b) { return preceding.ends_with(b); });
    if (counter.empty()) {
        // A number of more than one part, before a particle or alone: 2000,
        // 260から; with まん or おく mid-sentence too. Not にじゅう (二重), and
        // よ, く or し first (予選, 苦渋, 始終) only before まん or おく.
        const bool ends =
            rest.empty() || (any_prefix(rest, particles) && !rest.starts_with("の")) || // 五重塔.
            rest.starts_with("から") || rest.starts_with("まで") || rest.starts_with("たい");
        // A digit alone after a scale: しんどさん 震度3, れべるご レベル5.
        if (number.parts == 1 && number.value < 10 &&
            (preceding.ends_with("しんど") || (ends && preceding.ends_with("れべる")))) {
            return true;
        }
        if (number.parts < 2 || number.geminate || !led || !ends) {
            return false;
        }
        if (number.value >= 10000) {
            return true;
        }
        // Not a final に, which may be the particle (にじゅうにはらった is
        // 二重に払った), nor a digit and せん alone (ろくせん may be 6戦).
        const bool thousand = number.parts == 2 && (number.last == "せん" || number.last == "ぜん");
        return boundary && number.value != 20 && !(number.restricted && number.first != "に") &&
               number.last != "に" && !thousand;
    }
    if (number.parts > 1) {
        return led;
    }
    // A digit or unit alone. After a time of day (よる, いま, ゆうがた, よなかの)
    // the hour.
    const bool hour =
        (counter == "じ" || counter == "じはん") &&
        std::any_of(std::begin(times_of_day), std::end(times_of_day), [&](auto time) {
            return preceding.ends_with(time) || preceding.ends_with(std::string(time) + "の");
        });
    if (hour && number.first != "し") {
        return true;
    }
    // よじ after any particle (火曜日の4時); くじ, a lottery too, before
    // から, まで, ごろ, すぎ or には (えいがはくじから 9時から).
    const auto then_text = rest.substr(counter.size());
    if (boundary && number.first == "よ" &&
        (counter == "じ" || counter == "じかん" || counter == "じはん")) {
        return true;
    }
    if (boundary && number.first == "く" && counter == "じ" &&
        (then_text.starts_with("から") || then_text.starts_with("まで") ||
         then_text.starts_with("ごろ") || then_text.starts_with("すぎ") ||
         then_text.starts_with("には") || then_text.starts_with("まえ"))) {
        return true;
    }
    // A word such as ごじ counts before another number: ごじごじゅっぷん, and
    // linked to one with the same counter, にじからさんじ (not にほんから
    // よじかん).
    auto after = rest.substr(counter.size());
    bool linked = false;
    for (const std::string_view link : {"から", "まで", "と"}) {
        if (after.starts_with(link)) {
            after.remove_prefix(link.size());
            linked = true;
            break;
        }
    }
    const auto then = parse(after, 0);
    const auto next_counter = then.parts ? counter_at(after.substr(then.end)) : std::string_view{};
    const bool rate = !next_counter.empty() && (!linked || next_counter.starts_with(counter));
    const auto whole = std::string(text.substr(at, number.end - at)) + std::string(counter);
    if ((number.value < 10 &&
         std::find(std::begin(longer_only), std::end(longer_only), counter) != std::end(longer_only)) ||
        (!rate && std::find(std::begin(words), std::end(words), whole) != std::end(words))) {
        return false;
    }
    if (number.restricted) {
        const bool two_counter = std::find(std::begin(two_counters), std::end(two_counters), counter) !=
                                 std::end(two_counters);
        if (number.first == "に") {
            return (two_counter || rate) && (start || after_two());
        }
        return (start || after_word()) && // もうよねん, あとくじかん.
               std::any_of(std::begin(restricted_counters), std::end(restricted_counters),
                           [&](const auto& pair) {
                               return pair.first == number.first && pair.second == counter;
                           });
    }
    // ひゃく or せん alone before these only, as Mozc writes it otherwise
    // (専念, 繊細): 100円, 1000人.
    constexpr std::string_view unit_counters[] = {"にん", "ぴーす", "めーとる", "きろ", "ぐらむ",
                                                  "まい", "こ",     "てん",     "にち"};
    if (number.value >= 100) {
        // Not in an idiom of numbers: 百人一首.
        return std::find(std::begin(unit_counters), std::end(unit_counters), counter) !=
                   std::end(unit_counters) &&
               boundary && !parse(rest.substr(counter.size()), 0).parts;
    }
    return boundary;
}
// Counters for numbers Mozc already reads as numbers (numeral_digits()),
// which in kana alone start too many words: 三件, 三桁, 三割, 五足, 二着.
constexpr std::string_view numeral_counters[] = {
    "ちょうめ", "ごうしつ", "だんめ", "りょうめ", "じげんめ", "せいき", "けん", "けた", "わり", "そく",
    "ちゃく",   "とう",     "しゅ",   "じょう",   "れん",     "わ",     "かん", "もじ", "はこ", "えき",
    "はく",     "ぱく",     "めい",   "だん",     "ひょう",   "しょう", "はい", "たい", "い"};
// Their words with one digit, kept: 一体, 一生, 一種, 一段と, 一貫, 一連.
constexpr std::string_view numeral_words[] = {"いったい", "いっしょう", "いっしゅ", "いちだん",
                                              "いっかん", "いちれん",   "いちい",   "いっとう"};
// The value of the kanji numerals `text` starts with (四, 二百六十, 二万) and
// their length in bytes; {0, 0} for none, or for digits in a row (二〇二六).
std::pair<long long, std::size_t> kanji_number(std::string_view text) {
    constexpr std::string_view digits[] = {"〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    constexpr std::pair<std::string_view, int> units[] = {{"十", 10}, {"百", 100}, {"千", 1000}};
    constexpr std::pair<std::string_view, long long> large[] = {{"万", 10000}, {"億", 100000000}};
    long long total = 0, group = 0;
    int digit = -1;
    std::size_t at = 0, end = 0;
    while (at < text.size()) {
        const auto rest = text.substr(at);
        const auto d = std::find_if(std::begin(digits), std::end(digits),
                                    [&](auto k) { return rest.starts_with(k); });
        if (d != std::end(digits)) {
            if (digit >= 0) {
                return {};
            }
            digit = static_cast<int>(d - std::begin(digits));
            at += d->size();
            end = at;
            continue;
        }
        const auto u = std::find_if(std::begin(units), std::end(units),
                                    [&](auto k) { return rest.starts_with(k.first); });
        if (u != std::end(units)) {
            group += (digit > 0 ? digit : 1) * u->second;
            digit = -1;
            at += u->first.size();
            end = at;
            continue;
        }
        const auto l = std::find_if(std::begin(large), std::end(large),
                                    [&](auto k) { return rest.starts_with(k.first); });
        if (l != std::end(large) && (group || digit > 0)) {
            total += (group + std::max(digit, 0)) * l->second;
            group = 0;
            digit = -1;
            at += l->first.size();
            end = at;
            continue;
        }
        break;
    }
    const auto value = total + group + std::max(digit, 0);
    return value ? std::pair{value, end} : std::pair<long long, std::size_t>{};
}
// Digits one by one from `at`, as a decimal's or a phone number's: ごきゅう
// in いってんごきゅう. ぜろ and まる are 0 here.
std::vector<std::pair<std::size_t, std::string>> digit_run(std::string_view text, std::size_t at) {
    constexpr std::pair<std::string_view, char> digits[] = {
        {"ぜろ", '0'}, {"まる", '0'}, {"いち", '1'}, {"に", '2'},   {"さん", '3'},  {"よん", '4'},
        {"ご", '5'},   {"ろく", '6'}, {"なな", '7'}, {"はち", '8'}, {"きゅう", '9'}};
    std::vector<std::pair<std::size_t, std::string>> runs; // Each end, shortest first.
    std::string run;
    while (at < text.size()) {
        const auto found = std::find_if(std::begin(digits), std::end(digits), [&](const auto& d) {
            return text.substr(at).starts_with(d.first);
        });
        if (found == std::end(digits)) {
            break;
        }
        run += found->second;
        at += found->first.size();
        runs.emplace_back(at, run);
    }
    return runs;
}
// A decimal from `at`, number てん digits, before a counter: いってんごばい is
// 1.5ばい, よんじゅうにてんいちきゅうごきろ 42.195きろ. Empty if none.
std::pair<std::size_t, std::string> decimal(std::string_view text, std::size_t at) {
    constexpr std::string_view point = "てん";
    std::vector<std::pair<std::size_t, long long>> wholes; // Each number's end and value.
    for (const auto& number : parses(text, at)) {
        wholes.emplace_back(number.end, number.value);
    }
    for (const std::string_view zero : {"れい", "ぜろ"}) { // れいてんさん is 0.3.
        if (text.substr(at).starts_with(zero)) {
            wholes.emplace_back(at + zero.size(), 0);
        }
    }
    for (const auto& [end, value] : wholes) {
        if (!text.substr(end).starts_with(point)) {
            continue;
        }
        const auto runs = digit_run(text, end + point.size());
        for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
            // Not てん again: にてんさんてん is 二転三転.
            if (const auto counter = counter_at(text.substr(run->first));
                !counter.empty() && counter != point) {
                return {run->first + counter.size(),
                        std::to_string(value) + "." + run->second + std::string(counter)};
            }
        }
    }
    return {};
}
// A phone or emergency number spelled digit by digit, three or more, with a
// 0 among them or ばん after them: ぜろきゅうぜろ is 090, いちいちきゅうばん
// 119ばん.
std::pair<std::size_t, std::string> digit_string(std::string_view text, std::size_t at) {
    const auto runs = digit_run(text, at);
    for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
        if (run->second.size() >= 3 && (run->second.find('0') != std::string::npos ||
                                        text.substr(run->first).starts_with("ばん"))) {
            return *run;
        }
    }
    return {};
}
// Days spelled as words that are days alone, without a month before them.
constexpr std::pair<std::string_view, int> lone_days[] = {
    {"にじゅうよっか", 24}, {"じゅうよっか", 14}, {"はつか", 20}, {"よっか", 4}, {"ふつか", 2}};
} // namespace numbers
std::string numbers_as_digits(const std::string& reading) {
    using namespace numbers;
    const std::string_view text(reading);
    std::string result;
    bool month = false, dated = false; // After a month; after a day.
    for (std::size_t i = 0; i < text.size();) {
        // A day after a month, with の too (ごがつのみっか), or after another
        // day and から or まで (みっかからいつかまで).
        if (month) {
            month = false;
            std::string_view link;
            for (const std::string_view l : {"の", "から", "まで", "と", "や"}) {
                if (text.substr(i).starts_with(l)) {
                    link = l;
                    break;
                }
            }
            const auto at = i + link.size();
            const auto day = std::find_if(std::begin(days), std::end(days), [&](const auto& d) {
                return text.substr(at).starts_with(d.first);
            });
            if (day != std::end(days) && (link.empty() || link == "の" || dated)) {
                result += std::string(link) + std::to_string(day->second) + "にち";
                i = at + day->first.size();
                month = dated = true; // Another day may follow: から いつか まで.
                continue;
            }
        }
        dated = false;
        // At the start or after a particle: a decimal, a number digit by
        // digit, a day.
        const auto preceding = text.substr(0, i);
        const bool open = i == 0 ||
                          std::any_of(std::begin(boundaries), std::end(boundaries),
                                      [&](auto b) { return preceding.ends_with(b); }) ||
                          std::any_of(std::begin(number_words), std::end(number_words),
                                      [&](auto w) { return preceding.ends_with(w); });
        if (auto [end, digits] = decimal(text, i); !digits.empty()) {
            result += digits;
            i = end;
            continue;
        }
        if (auto [end, digits] = digit_string(text, i); open && !digits.empty()) {
            result += digits;
            i = end;
            continue;
        }
        if (open) {
            const auto day =
                std::find_if(std::begin(lone_days), std::end(lone_days),
                             [&](const auto& d) { return text.substr(i).starts_with(d.first); });
            // Before a particle or 間, 目, 後, 前 only: not 二日酔い, 四日市.
            const auto rest =
                day != std::end(lone_days) ? text.substr(i + day->first.size()) : std::string_view{};
            constexpr std::string_view day_ends[] = {"は",   "が",   "も",   "を", "の", "に",   "で",
                                                     "から", "まで", "かん", "め", "ご", "まえ", "、"};
            if (day != std::end(lone_days) && (rest.empty() || any_prefix(rest, day_ends))) {
                result += std::to_string(day->second) + "にち";
                i += day->first.size();
                month = dated = true;
                continue;
            }
            // Any day before の and a time of day: いつかのごごさんじ.
            const auto any_day = std::find_if(std::begin(days), std::end(days), [&](const auto& d) {
                return text.substr(i).starts_with(d.first);
            });
            if (any_day != std::end(days)) {
                const auto after_day = text.substr(i + any_day->first.size());
                if (after_day.starts_with("の") &&
                    std::any_of(std::begin(times_of_day), std::end(times_of_day), [&](auto time) {
                        return after_day.substr(std::string_view("の").size()).starts_with(time);
                    })) {
                    result += std::to_string(any_day->second) + "にち";
                    i += any_day->first.size();
                    continue;
                }
            }
        }
        const auto found = parses(text, i);
        const auto number = found.empty() ? Number{} : found.back();
        bool done = false;
        for (auto it = found.rbegin(); it != found.rend() && !done; ++it) {
            const auto counter = counter_at(text.substr(it->end));
            if ((it == found.rbegin() || !counter.empty()) && counts(text, i, *it, counter)) {
                result += written(it->value);
                month = counter == "がつ" || counter == "にち";
                dated = counter == "にち";
                result += counter;
                i = it->end + counter.size();
                done = true;
            }
        }
        if (done) {
            continue;
        }
        if (number.parts) {
            // Kept as typed, whole: no number within it either (しじゅうくにち).
            result.append(text.substr(i, number.end - i));
            i = number.end;
            continue;
        }
        const auto next = static_cast<std::size_t>(g_utf8_next_char(text.data() + i) - text.data());
        result.append(text.substr(i, next - i));
        i = next;
    }
    return result;
}
// The reading with each ん before a vowel or や, ゆ, よ read the other way
// romaji's nn allows: こんいちは as こんにちは, ほんや as ほんにゃ.
std::string carried_n(const std::string& reading) {
    static const std::map<std::string, std::string> carried{
        {"あ", "な"}, {"い", "に"},   {"う", "ぬ"},   {"え", "ね"},
        {"お", "の"}, {"や", "にゃ"}, {"ゆ", "にゅ"}, {"よ", "にょ"}};
    std::string result;
    bool after_n = false;
    for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
        const std::string kana(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        const auto found = after_n ? carried.find(kana) : carried.end();
        result += found != carried.end() ? found->second : kana;
        after_n = kana == "ん";
    }
    return result;
}
// Which characters of carried_n(reading) it changed, one flag each.
std::vector<bool> carried_marks(const std::string& reading) {
    std::vector<bool> marks;
    bool after_n = false;
    for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
        const std::string kana(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        const auto other = carried_n("ん" + kana);
        const bool changed = after_n && other != "ん" + kana;
        const auto count = changed ? g_utf8_strlen(other.c_str(), -1) - 1 : 1;
        marks.insert(marks.end(), static_cast<std::size_t>(count), changed);
        after_n = kana == "ん";
    }
    return marks;
}
// Full-width forms as their narrow ones: ？ as ?, ￥ as ¥.
std::string half_width(const std::string& text) {
    std::string result;
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        auto c = g_utf8_get_char(p);
        if (c >= 0xff01 && c <= 0xff5e) {
            c -= 0xfee0;
        } else if (c == 0xffe5) {
            c = 0xa5;
        }
        char buffer[8]{};
        result.append(buffer, static_cast<std::size_t>(g_unichar_to_utf8(c, buffer)));
    }
    return result;
}
// A personality's record: its base file and journal, as Anthy names them.
std::filesystem::path record_file(const std::filesystem::path& directory, int part,
                                  const std::string& id) {
    return directory / ("last-record" + std::to_string(part) + "_" + id + ".utf8");
}
} // namespace
bool configure_anthy(AnthyLearning learning) {
    auto& state = anthy();
    std::lock_guard lock(state.settings);
    state.learning = learning;
    state.configured = true;
    return state.started && *state.started != learning;
}
bool anthy_configured() {
    auto& state = anthy();
    std::lock_guard lock(state.settings);
    return state.configured;
}
void restart_anthy(bool clear) {
    auto& state = anthy();
    std::lock_guard lock(state.calls);
    if (state.api) {
        for (auto* converter : state.converters) {
            converter->release();
        }
        state.api->quit();
        std::lock_guard settings(state.settings);
        state.started.reset();
    }
    if (clear) {
        // The record itself: a base file and its journal, and their lock.
        // The stock keyboard's own stays as it is.
        const auto home = [&] {
            std::lock_guard settings(state.settings);
            return learning_home(state.learning);
        }();
        std::error_code ignored;
        for (const char* prefix : {"last-record1_", "last-record2_"}) {
            for (const char* suffix : {".utf8", ""}) {
                std::filesystem::remove(home / ".anthy" / (prefix + std::string(learning_id) + suffix),
                                        ignored);
            }
        }
        std::filesystem::remove(home / ".anthy" / ("lock-file_" + std::string(learning_id)), ignored);
    }
    if (state.api) {
        try {
            start(state);
        } catch (const std::exception&) {
            // The next conversion loads it again and reports why it cannot.
            dlclose(state.api->handle);
            state.api.reset();
        }
    }
}
void set_anthy_dictionary(const std::filesystem::path& dictionary) {
    auto& state = anthy();
    std::lock_guard lock(state.settings);
    state.dictionary = dictionary.string();
}
std::filesystem::path default_anthy_home() {
    return learning_home({});
}
bool anthy_learned() {
    auto& state = anthy();
    const auto home = [&] {
        std::lock_guard lock(state.settings);
        return learning_home(state.learning);
    }();
    std::error_code error;
    for (const int part : {1, 2}) {
        if (const auto size =
                std::filesystem::file_size(record_file(home / ".anthy", part, learning_id), error);
            !error && size > 0) {
            return true;
        }
    }
    return false;
}
bool import_stock_learning(const std::filesystem::path& marker) {
    std::error_code error;
    if (std::filesystem::exists(marker, error)) {
        return false;
    }
    // Once only, whatever there was to copy: a record cleared later stays
    // empty, and later stock learning stays the stock keyboard's.
    std::filesystem::create_directories(marker.parent_path(), error);
    std::ofstream(marker).close();
    auto& state = anthy();
    const auto directory = [&] {
        std::lock_guard lock(state.settings);
        return learning_home(state.learning) / ".anthy";
    }();
    for (const int part : {1, 2}) {
        if (std::filesystem::exists(record_file(directory, part, learning_id), error)) {
            return false; // A record of our own already.
        }
    }
    // Under Anthy's own lock, a lock on the lock file's first byte, which a
    // writing stock keyboard holds: the copy is never half written. Read
    // only: the stock keyboard's files stay exactly as they are.
    const auto lock_file = directory / ("lock-file_" + std::string(stock_id));
    const int lock = open(lock_file.c_str(), O_RDONLY | O_CLOEXEC);
    if (lock >= 0) {
        struct flock range{};
        range.l_type = F_RDLCK;
        range.l_whence = SEEK_SET;
        range.l_len = 1;
        while (fcntl(lock, F_SETLKW, &range) == -1 && errno == EINTR) {
        }
    }
    bool copied = false;
    for (const int part : {1, 2}) {
        const auto from = record_file(directory, part, stock_id);
        if (std::filesystem::exists(from, error) &&
            std::filesystem::copy_file(from, record_file(directory, part, learning_id), error)) {
            std::filesystem::permissions(record_file(directory, part, learning_id),
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, error);
            copied = true;
        }
    }
    if (lock >= 0) {
        close(lock); // Releases the lock.
    }
    return copied;
}
std::unique_ptr<JapaneseConverter> anthy_converter() {
    return std::make_unique<AnthyConverter>();
}
JapaneseComposer::JapaneseComposer(Factory factory) : factory_(std::move(factory)) {
}
JapaneseComposer::~JapaneseComposer() = default;
JapaneseConverter& JapaneseComposer::converter() {
    if (!converter_) {
        converter_ = factory_ ? factory_() : anthy_converter();
    }
    return *converter_;
}
namespace {
// Particles a word's usual form may come before, kept as typed: いくね as
// 行くね, いくから as 行くから.
constexpr std::string_view word_particles[] = {
    "は",   "が",   "を",   "に",   "で",   "と",   "も",   "の",   "へ",    "や",
    "か",   "ね",   "よ",   "な",   "わ",   "ぞ",   "ぜ",   "だ",   "から",  "かも",
    "けど", "まで", "だけ", "ので", "のに", "よね", "かな", "って", "じゃん"};
// Of those, the ones a noun comes before, where a word the list keeps in kana
// may be meant in kanji: うんがいい is 運がいい, while うん alone stays うん.
// And って, which also ends a verb's て form: はいって is 入って.
constexpr std::string_view noun_particles[] = {"は", "が", "を", "に",   "で",   "と",  "も",
                                               "の", "へ", "や", "から", "まで", "って"};
bool noun_particle(std::string_view particle) {
    return std::find(std::begin(noun_particles), std::end(noun_particles), particle) !=
           std::end(noun_particles);
}
// Hiragana only, as the list keeps interjections and such words: うん, いい.
bool hiragana_only(const std::string& text) {
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        const auto c = g_utf8_get_char(p);
        if ((c < 0x3041 || c > 0x3096) && c != 0x30FC) {
            return false;
        }
    }
    return !text.empty();
}
// A katakana word, as Anthy reads loanwords: カレー, コーヒー.
bool katakana_word(const std::string& text) {
    bool letters = false;
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        const auto c = g_utf8_get_char(p);
        if ((c < 0x30A1 || c > 0x30FA) && c != 0x30FC) {
            return false;
        }
        letters |= c != 0x30FC;
    }
    return letters;
}
// Long enough to be read as a loanword: three kana or more, or with ー. A
// shorter reading may be a native word's start (さむ for 寒い, not サム).
bool loanword_length(const std::string& reading) {
    return g_utf8_strlen(reading.c_str(), -1) >= 3 || reading.find("ー") != std::string::npos;
}
// Particles ending the segment before a word that the phrase list may give
// the word another form after, in entries marked 〜 that only apply so:
// 〜があう 〜が合う makes 時間が合う, where あう alone is 会う.
constexpr std::string_view case_particles[] = {"が", "を", "に", "と", "で", "は", "も"};
constexpr std::string_view context_mark = "〜";
// The particles a kana word Anthy left at a segment's end may follow
// (usual_tail). Not で, which できる and its forms begin with (ご飯できた),
// nor て, where 〜ていく and 〜てくる are written in kana.
constexpr std::string_view tail_particles[] = {"に", "へ", "と", "から"};
} // namespace
std::vector<std::string> JapaneseComposer::listed_forms(const std::string& key,
                                                        const std::string& prefix) const {
    // In a sentence the form last picked for a listed word leads, then
    // set_preferred()'s, whose order, most picked first, is for a reading on
    // its own.
    std::vector<std::string> forms;
    auto listed = preferred_ ? preferred_(key) : std::vector<std::string>{};
    if (listed.empty()) {
        return forms;
    }
    if (auto pick = word_pick_ ? word_pick_(key) : std::string{}; !pick.empty()) {
        listed.insert(listed.begin(), std::move(pick));
    }
    for (const auto& form : listed) {
        if (std::string_view(form).starts_with(prefix) && form.size() > prefix.size() &&
            std::find(forms.begin(), forms.end(), form.substr(prefix.size())) == forms.end()) {
            forms.push_back(form.substr(prefix.size()));
        }
    }
    return forms;
}
std::optional<std::pair<std::string, std::vector<std::string>>>
JapaneseComposer::word_forms(const std::string& word, const Place& place) const {
    // A context entry's forms lose their mark and particle.
    const auto forms_of = [&](const std::string& key, const std::string& prefix) {
        return listed_forms(key, prefix);
    };
    for (const auto particle : case_particles) {
        if (std::string_view(place.before).ends_with(particle)) {
            const auto prefix = std::string(context_mark) + std::string(particle);
            if (auto forms = forms_of(prefix + word, prefix); !forms.empty()) {
                return std::pair{prefix + word, std::move(forms)};
            }
            continue; // Its own particle again begins another word: 駅で|であった.
        }
        // Or the particle begins this segment, after another, at the phrase's
        // end: 場所|であう, but 学校であった話 keeps Anthy's.
        if (!place.before.empty() && place.last && std::string_view(word).starts_with(particle)) {
            const std::string mark(context_mark);
            if (auto forms = forms_of(mark + word, mark); !forms.empty()) {
                return std::pair{mark + word, std::move(forms)};
            }
        }
    }
    if (auto forms = forms_of(word, {}); !forms.empty()) {
        return std::pair{word, std::move(forms)};
    }
    return {};
}
bool JapaneseComposer::common_word(const std::string& reading, const std::string& text) const {
    if (!words_) {
        return false;
    }
    // Each way to read the text as a word of the reading's start, with the
    // rest of the reading in kana after it.
    const char* end = reading.c_str() + reading.size();
    for (const char* p = g_utf8_next_char(reading.c_str()); p <= end; p = g_utf8_next_char(p)) {
        const auto at = static_cast<std::size_t>(p - reading.c_str());
        const auto rest = reading.substr(at);
        if (std::string_view(text).ends_with(rest)) {
            const auto head = text.substr(0, text.size() - rest.size());
            for (const auto& word : words_->exact(reading.substr(0, at), 16)) {
                if (word == head) {
                    return true;
                }
            }
        }
        if (p == end) {
            break;
        }
    }
    return false;
}
std::optional<std::pair<std::string, std::string>>
JapaneseComposer::particle_split(const std::string& reading, const std::string& text,
                                 const Place& place) const {
    for (const auto particle : word_particles) {
        if (reading.size() > particle.size() && std::string_view(reading).ends_with(particle)) {
            const auto word = reading.substr(0, reading.size() - particle.size());
            if (g_utf8_strlen(word.c_str(), -1) < 2) {
                continue;
            }
            if (auto found = word_forms(word, place)) {
                // Where Anthy reads the kana as another word, a common one
                // stays: くるまで is 車で, not 来るまで, but いくよ 行くよ, not 幾夜.
                if ((hiragana_only(found->second.front()) && noun_particle(particle)) ||
                    (!std::string_view(text).ends_with(particle) &&
                     (place.context || common_word(reading, text)))) {
                    return {};
                }
                return std::pair{word, std::string(particle)};
            }
        }
    }
    return {};
}
std::vector<std::string> JapaneseComposer::usual_forms(const std::string& reading,
                                                       const std::string& text,
                                                       const Place& place) const {
    if (auto found = word_forms(reading, place)) {
        return found->second;
    }
    if (const auto split = particle_split(reading, text, place)) {
        auto forms = word_forms(split->first, place)->second;
        for (auto& form : forms) {
            form += split->second;
        }
        return forms;
    }
    return {};
}
std::optional<JapaneseComposer::Tail> JapaneseComposer::tail_word(const std::string& reading,
                                                                  const std::string& text,
                                                                  const Place& place) const {
    // Not in a word the whole reading is, as Anthy kept it: からかう stays.
    if (!preferred_) {
        return {};
    }
    if (words_) {
        const auto whole = words_->exact(reading, 16);
        if (std::find(whole.begin(), whole.end(), text) != whole.end()) {
            return {};
        }
    }
    for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
        const auto at = static_cast<std::size_t>(p - reading.c_str());
        auto word = reading.substr(at);
        if (g_utf8_strlen(word.c_str(), -1) < 2) {
            break;
        }
        const auto after = [&](std::string_view particle) {
            return at >= particle.size() &&
                   std::string_view(reading).substr(at - particle.size(), particle.size()) == particle;
        };
        // A context entry for the particle before it, at the phrase's end:
        // 仕事であった as 仕事で会った, but 学校であった話 keeps Anthy's.
        for (const auto particle : case_particles) {
            const auto kana = std::string(particle) + word;
            if (place.last && after(particle)) {
                if (auto forms =
                        listed_forms(std::string(context_mark) + kana, std::string(context_mark));
                    !forms.empty()) {
                    return Tail{std::string(context_mark) + kana, kana, std::move(forms)};
                }
            }
        }
        for (const auto particle : tail_particles) {
            // Not について, "about", which is no 着いて.
            if (particle == "に" && std::string_view(word).starts_with("ついて")) {
                continue;
            }
            if (after(particle)) {
                if (auto found = word_forms(word, {})) {
                    return Tail{word, word, std::move(found->second)};
                }
            }
        }
    }
    return {};
}
std::string JapaneseComposer::usual_tail(const std::string& reading, const std::string& text,
                                         const Place& place) const {
    // A word Anthy left in kana after one of the tail particles takes its
    // usual form: 食べにいこう as 食べに行こう, 学校からかえる 学校から帰る.
    const auto tail = tail_word(reading, text, place);
    if (!tail || !std::string_view(text).ends_with(tail->kana) || text.size() == tail->kana.size()) {
        return {};
    }
    const auto& form = tail->forms.front();
    return form != tail->kana ? text.substr(0, text.size() - tail->kana.size()) + form : std::string{};
}
std::optional<std::pair<std::string, std::string>>
JapaneseComposer::word_choice(const std::string& reading, const std::string& text,
                              const Place& place) const {
    // A context entry's key keeps its mark and particle in the choice:
    // 〜があう, 〜が合う.
    const auto chosen = [](const std::string& key, const std::string& word, const std::string& written) {
        return std::pair{key, key.substr(0, key.size() - word.size()) + written};
    };
    if (auto found = word_forms(reading, place)) {
        return chosen(found->first, reading, text);
    }
    if (const auto split = particle_split(reading, text, place)) {
        if (text.size() > split->second.size() && std::string_view(text).ends_with(split->second)) {
            return chosen(word_forms(split->first, place)->first, split->first,
                          text.substr(0, text.size() - split->second.size()));
        }
        return {};
    }
    // A word after a tail particle, as usual_tail() finds it, kept in kana
    // or in one of its forms; a context entry's choice keeps its mark.
    if (auto tail = tail_word(reading, text, place)) {
        tail->forms.push_back(tail->kana);
        const auto mark =
            tail->key.size() > tail->kana.size() ? std::string(context_mark) : std::string{};
        for (const auto& form : tail->forms) {
            if (text.size() > form.size() && std::string_view(text).ends_with(form)) {
                return std::pair{tail->key, mark + form};
            }
        }
    }
    return {};
}
std::vector<std::pair<std::string, std::string>> JapaneseComposer::word_choices() const {
    std::vector<std::pair<std::string, std::string>> result;
    if (!converting() || predicted_) {
        return result;
    }
    Place place;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& segment = segments_[i];
        place.last = i + 1 == segments_.size();
        if (auto choice =
                word_choice(segment.reading,
                            segment.candidates[static_cast<std::size_t>(segment.selected)], place)) {
            result.push_back(std::move(*choice));
        }
        place.before = segment.reading;
    }
    return result;
}
std::vector<JapaneseComposer::Segment>
JapaneseComposer::imported(std::vector<JapaneseConverter::Segment> segments) const {
    std::vector<Segment> result;
    Place place; // Where the segment stands, for a word's context.
    place.context = converter_ && converter_->weighs_context();
    for (auto& from : segments) {
        place.last = &from == &segments.back();
        if (from.candidates.empty()) {
            throw std::runtime_error("No conversion candidates");
        }
        Segment segment{std::move(from.reading), std::move(from.candidates)};
        // Anthy offers ！ and ？ half-width first, and 」 as 』; a phone keeps
        // symbols as typed and offers the others after them.
        if (symbols_only(segment.reading)) {
            auto& candidates = segment.candidates;
            const auto typed = std::find(candidates.begin(), candidates.end(), segment.reading);
            if (typed == candidates.end()) {
                candidates.insert(candidates.begin(), segment.reading);
            } else {
                std::rotate(candidates.begin(), typed, typed + 1);
            }
            // Its half-width form next, which Anthy may not offer (¥ for ￥).
            if (const auto narrow = half_width(segment.reading);
                std::find(candidates.begin(), candidates.end(), narrow) == candidates.end()) {
                candidates.insert(candidates.begin() + 1, narrow);
            }
        }
        // A word's usual form, as the phrase list gives it, leads its
        // segment, before a particle too: いく as 行く, いくね as 行くね, so
        // すぐいく is すぐ行く where Anthy offers 育 first.
        auto forms = word_forms_ ? usual_forms(segment.reading, segment.candidates.front(), place)
                                 : std::vector<std::string>{};
        if (forms.empty() && word_forms_) {
            if (auto tail = usual_tail(segment.reading, segment.candidates.front(), place);
                !tail.empty()) {
                forms.push_back(std::move(tail));
            }
        }
        if (!forms.empty()) {
            auto& candidates = segment.candidates;
            for (const auto& form : forms) {
                std::erase(candidates, form);
            }
            candidates.insert(candidates.begin(), forms.begin(), forms.end());
        }
        // The segment's reading in both scripts, after Anthy's own candidates.
        for (const auto& script :
             {in_script(segment.reading, false), in_script(segment.reading, true)}) {
            if (!script.empty() && segment.candidates.size() < 130 &&
                std::find(segment.candidates.begin(), segment.candidates.end(), script) ==
                    segment.candidates.end()) {
                segment.candidates.push_back(script);
            }
        }
        place.before = segment.reading;
        result.push_back(std::move(segment));
    }
    return result;
}
std::string JapaneseComposer::Stretch::apply(const std::string& text) const {
    if (final.empty()) {
        return text + marks;
    }
    for (const auto& vowel : {final, in_script(final, true)}) {
        if (text.size() > vowel.size() && std::string_view(text).ends_with(vowel)) {
            return text.substr(0, text.size() - vowel.size()) + marks + vowel;
        }
    }
    return {};
}
// Mozc reads a number spelled in kana as one where the sentence calls for it
// (もう|四年も, ちょうど|十|年, 約|二百六十|年), while numbers_as_digits() takes
// one mid-sentence only after a particle. Where Mozc's first candidate starts
// with kanji numerals and it offers the digits too, these come first, under
// the same rules as numbers_as_digits() at a reading's start, but for a unit
// alone (百歳 as 100歳): a counter after them, here or opening the next
// segment, or a number of more than one part (二千), and no word (一番, 一日).
void JapaneseComposer::numeral_digits(std::vector<Segment>& segments) {
    using namespace numbers;
    constexpr std::string_view ordinal = "第", ordinal_kana = "だい";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        auto& candidates = segments[i].candidates;
        std::string_view front = candidates.front(), reading = segments[i].reading;
        std::string prefix;
        if (front.starts_with(ordinal) && reading.starts_with(ordinal_kana)) {
            prefix = ordinal;
            front.remove_prefix(ordinal.size());
            reading.remove_prefix(ordinal_kana.size());
        }
        const auto [value, length] = kanji_number(front);
        if (!value) {
            continue;
        }
        // The spelling of the same number (五 in 五重塔 is no ごじゅう), then
        // the counter after it, in this segment or the next.
        const auto spelled = parses(reading, 0);
        const auto same = std::find_if(spelled.rbegin(), spelled.rend(),
                                       [&](const auto& n) { return n.value == value; });
        if (same == spelled.rend()) {
            continue;
        }
        const auto number = *same;
        const auto counter_in = [&](std::string_view rest) {
            auto counter = counter_at(rest);
            for (const auto c : numeral_counters) {
                if (counter.empty() && rest.starts_with(c)) {
                    counter = c;
                }
            }
            return counter;
        };
        auto counter = counter_in(reading.substr(number.end));
        // 五分 read ごぶ after a temperature: 38度5分.
        const std::string_view previous =
            i > 0 ? std::string_view(segments[i - 1].candidates.front()) : "";
        if (counter.empty() && previous.ends_with("度") &&
            reading.substr(number.end).starts_with("ぶ")) {
            counter = "ぶ";
        }
        auto next = i + 1; // The segment after the number and its counter.
        if (counter.empty() && number.end == reading.size() && next < segments.size()) {
            counter = counter_in(segments[next].reading);
            next += !counter.empty();
        }
        const auto rest = std::string(front.substr(length));
        const auto whole = std::string(reading.substr(0, number.end)) + std::string(counter);
        const auto listed = [](const auto& list, auto item) {
            return std::find(std::begin(list), std::end(list), item) != std::end(list);
        };
        // A word such as 一日 counts before another number: 1日3回.
        const bool rate = next < segments.size() && parse(segments[next].reading, 0).parts &&
                          kanji_number(segments[next].candidates.front()).first;
        // A score's second number: 3対|一で.
        const bool score = previous.size() > std::string_view("対").size() && previous.ends_with("対") &&
                           std::isdigit(static_cast<unsigned char>(
                               previous[previous.size() - std::string_view("対").size() - 1]));
        if ((counter.empty() && !score && (number.parts < 2 || has_kanji(rest))) || // Not 十五夜.
            (number.parts < 2 && listed(longer_only, counter)) ||                   // 一度, 一点物.
            (listed(words, whole) && !rate) || (number.parts < 2 && listed(numeral_words, whole)) ||
            (number.first == "し" && counter != "がつ")) {
            continue;
        }
        // As chat writes it, 2万 and 80億; else all digits, as Mozc offers.
        auto mixed = written(value);
        for (const auto& [kana, kanji] : {std::pair{"おく", "億"}, std::pair{"まん", "万"}}) {
            if (const auto at = mixed.find(kana); at != std::string::npos) {
                mixed.replace(at, std::string_view(kana).size(), kanji);
            }
        }
        bool moved = false;
        for (const auto& digits : {mixed, std::to_string(value)}) {
            const auto text = prefix + digits + rest;
            const auto found = std::find(candidates.begin(), candidates.end(), text);
            if (found != candidates.end()) {
                std::rotate(candidates.begin(), found, found + 1);
                moved = true;
                break;
            }
        }
        // Mozc offers 二万 in no digits but 20000: 2万 then, as chat writes it.
        if (!moved && value >= 10000 && mixed != std::to_string(value)) {
            candidates.insert(candidates.begin(), prefix + mixed + rest);
        }
    }
    floors(segments);
}
// A floor where Mozc reads 回: after 地下, or before a room, a lift or a floor's
// place (6階のエレベーター, 5階の502号室, 地下2階).
void JapaneseComposer::floors(std::vector<Segment>& segments) {
    constexpr std::string_view times = "回", floor = "階";
    constexpr std::string_view places[] = {
        "エレベーター", "部屋", "号室", "トイレ", "売り場",   "フロア", "ロビー", "教室", "会議室",
        "ベランダ",     "窓",   "建て", "受付",   "オフィス", "事務所", "席",     "店"};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        auto& candidates = segments[i].candidates;
        const std::string_view front = candidates.front();
        // 回 after the number, in this segment (6回の) or the one before (五|回の).
        std::size_t at = 0;
        while (at < front.size() && std::isdigit(static_cast<unsigned char>(front[at]))) {
            ++at;
        }
        at = at ? at : numbers::kanji_number(front).second;
        const std::string_view before =
            i > 0 ? std::string_view(segments[i - 1].candidates.front()) : "";
        const bool counted =
            at || (!before.empty() && (std::isdigit(static_cast<unsigned char>(before.back())) ||
                                       numbers::kanji_number(before).second == before.size()));
        if (!counted || !front.substr(at).starts_with(times) ||
            !(segments[i].reading.find("かい") != std::string::npos ||
              segments[i].reading.find("がい") != std::string::npos)) {
            continue;
        }
        auto rest = front.substr(at + times.size());
        const std::string_view next =
            i + 1 < segments.size() ? std::string_view(segments[i + 1].candidates.front()) : "";
        const auto starts_place = [&](std::string_view text) {
            while (!text.empty() && std::isdigit(static_cast<unsigned char>(text.front()))) {
                text.remove_prefix(1); // 502号室.
            }
            return std::any_of(std::begin(places), std::end(places),
                               [&](auto place) { return text.starts_with(place); });
        };
        const auto preceding =
            i > 1 && !at ? std::string_view(segments[i - 2].candidates.front()) : before;
        const bool underground = preceding.ends_with("地下") ||
                                 std::string_view(before).find("地下") != std::string_view::npos;
        if (underground ||
            starts_place(rest.starts_with("の") ? rest.substr(std::string_view("の").size()) : rest) ||
            (rest == "の" || rest.empty() ? starts_place(next) : false)) {
            auto text = std::string(front);
            text.replace(at, times.size(), floor);
            const auto found = std::find(candidates.begin(), candidates.end(), text);
            if (found != candidates.end()) {
                std::rotate(candidates.begin(), found, found + 1);
            }
        }
    }
}
// Verbs the object before them decides, where Mozc reads another verb of the
// same kana first: 雪|振ってきた as 雪降ってきた, 写真を|取って as 撮って, 傘|化して
// as 貸して, 服を|来て as 着て, ギター|引ける as 弾ける; and nouns the verb after
// them decides (風|引いた as 風邪引いた, 席が|出る as 咳が出る). Only to a
// candidate the converter offers.
void JapaneseComposer::object_verbs(std::vector<Segment>& segments) {
    struct Rule {
        std::string_view wrong, right;
        std::vector<std::string_view> nouns;
    };
    static const Rule rules[] = {
        {"振", "降", {"雪", "雨", "雹", "霰", "初雪", "大雨", "小雨"}},
        {"取", "撮", {"写真", "動画", "自撮り", "映像", "写メ", "プリ"}},
        {"化",
         "貸",
         {"傘", "お金", "金", "円", "手袋", "消しゴム", "鉛筆", "自転車", "ノート", "イヤホン", "本",
          "ペン", "充電器", "手", "漫画"}},
        {"来",
         "着",
         {"服", "浴衣", "着物", "制服", "コート", "シャツ", "水着", "スーツ", "ドレス", "セーター",
          "パーカー"}},
        {"書", "か", {"いびき", "汗", "恥", "あぐら"}},
        {"聞", "効", {"冷房", "暖房", "エアコン", "クーラー", "薬", "ブレーキ", "効果", "わさび"}},
        {"立", "た", {"時間", "月日", "年月"}},
        {"経", "立", {"予定", "計画", "目処", "目途", "見通し"}},
        {"引", "弾", {"ギター", "ピアノ", "ベース", "バイオリン", "鍵盤", "三味線"}},
        {"書", "描", {"絵", "イラスト", "似顔絵"}},
        {"占", "閉", {"窓", "ドア", "扉", "カーテン", "蓋", "シャッター"}},
        {"買", "飼", {"猫", "犬", "ペット", "金魚", "ハムスター", "うさぎ", "インコ", "メダカ"}},
        {"病", "や", {"雨", "雪"}},
        {"な", "鳴", {"電話", "スマホ", "ベル", "チャイム", "アラーム", "目覚まし", "インターホン"}},
        {"冷", "覚", {"目"}},
        {"話", "離", {"手", "目"}},
        {"炒", "痛", {"腰", "足", "膝", "肩", "首", "背中", "手首", "喉", "胸"}},
        {"乾", "渇", {"喉"}},
        {"写", "移", {"色", "風邪"}},
        {"変", "替", {"電球", "電池", "シーツ", "タオル", "替え"}},
        {"帰って",
         "返って",
         {"貸したら", "貸したのに", "返事", "連絡", "返信", "LINE", "メール", "お金"}},
        {"つ", "着", {"家", "駅", "空港", "会社", "学校", "ホテル", "会場", "現地", "目的地"}},
        {"早", "速", {"足"}},
        {"買", "勝", {"試合", "勝負", "決勝", "対戦"}},
        {"溜", "貯", {"お金", "ポイント", "貯金", "円"}},
    };
    const auto promote = [](std::vector<std::string>& candidates, const std::string& text) {
        const auto found = std::find(candidates.begin(), candidates.end(), text);
        if (found != candidates.end() && found != candidates.begin()) {
            std::rotate(candidates.begin(), found, found + 1);
        }
    };
    for (std::size_t i = 0; i < segments.size(); ++i) {
        // The noun: the segment before, or up to two further back before a
        // particle, across words that are no other object (この写真は|ちょうど|
        // 10年前に|撮った; not 雪の中で|手を|振った).
        std::vector<std::string_view> objects;
        for (std::size_t back = 1; back <= 3 && back <= i; ++back) {
            std::string_view before = segments[i - back].candidates.front();
            std::string_view particle;
            for (const std::string_view p : {"が", "を", "は", "も", "に"}) { // 雨に|降られた.
                if (before.size() > p.size() && before.ends_with(p)) {
                    before.remove_suffix(p.size());
                    particle = p;
                    break;
                }
            }
            // Further back only before が, を, は or も: 家に|いると|つい stays.
            if (back == 1 || (!particle.empty() && particle != "に")) {
                objects.push_back(before);
            }
            if (particle == "を") {
                break;
            }
        }
        const auto after = [&](const auto& nouns) {
            return std::any_of(objects.begin(), objects.end(), [&](auto object) {
                return std::any_of(nouns.begin(), nouns.end(),
                                   [&](auto noun) { return object.ends_with(noun); });
            });
        };
        auto& candidates = segments[i].candidates;
        // The first candidate afresh for each rule: one may have moved it.
        for (const auto& rule : rules) {
            std::string_view verb = candidates.front();
            // Or the noun and verb in one segment: 1000円化して.
            for (const auto noun : rule.nouns) {
                const auto at = verb.find(std::string(noun) + std::string(rule.wrong));
                if (at != std::string_view::npos) {
                    auto text = std::string(verb);
                    text.replace(at + noun.size(), rule.wrong.size(), rule.right);
                    promote(candidates, text);
                    break;
                }
            }
            // promote() rotates the strings the earlier view referred to.
            verb = candidates.front();
            if (i > 0 && verb.starts_with(rule.wrong) && after(rule.nouns)) {
                promote(candidates,
                        std::string(rule.right) + std::string(verb.substr(rule.wrong.size())));
            }
        }
        // A noun the verb after it decides: 風邪 before ひく, 歯を before
        // みがく, 帽子を before かぶる, 咳が before でる or とまる (席を出る
        // stays).
        static const std::array<std::array<std::string_view, 4>, 20> nouns{{
            {"ひ", "かぜ", "風", "風邪"},           {"みが", "はを", "葉", "歯"},
            {"かぶ", "ぼうしを", "防止", "帽子"},   {"で", "せきが", "席", "咳"},
            {"とま", "せきが", "席", "咳"},         {"き", "かみを", "神", "髪"},
            {"そ", "かみを", "神", "髪"},           {"の", "かみが", "神", "髪"},
            {"もら", "かきを", "書き", "柿"},       {"た", "かきを", "書き", "柿"},
            {"くさ", "ねが", "音", "根"},           {"はや", "かぜが", "風", "風邪"},
            {"やす", "かぜで", "風", "風邪"},       {"あが", "ていじで", "提示", "定時"},
            {"かえ", "ていじで", "提示", "定時"},   {"はし", "こうえんを", "公演", "公園"},
            {"あそ", "こうえんで", "公演", "公園"}, {"たの", "かんじを", "感じ", "幹事"},
            {"しんど", "じしんは", "自身", "地震"}, {"の", "いっぱい", "いっぱい", "一杯"},
        }};
        for (const auto& [verb_start, noun_reading, wrong, right] : nouns) {
            if (i == 0) {
                break;
            }
            const auto& noun = segments[i - 1].candidates.front();
            if (std::string_view(segments[i].reading).starts_with(verb_start) &&
                std::string_view(segments[i - 1].reading).starts_with(noun_reading) &&
                std::string_view(noun).starts_with(wrong) &&
                !std::string_view(noun).starts_with(right)) {
                promote(segments[i - 1].candidates, std::string(right) + noun.substr(wrong.size()));
            }
        }
    }
}
// Words chat writes in kana where Mozc writes kanji: お伝えください, なし,
// よかった, できる, もらう. Where a segment's first candidate has one and the
// converter offers it in kana too, that comes first.
void JapaneseComposer::chat_kana(std::vector<Segment>& segments) {
    // `verb`: only before a verb's ending in kana, not in a noun (上出来,
    // 頂き物). `alone`: not right after a kanji, as in 仲良く.
    struct Kana {
        std::string_view kanji, kana;
        bool verb{}, alone{};
    };
    static const Kana words[] = {{"下さ", "くださ"},
                                 {"無し", "なし"},
                                 {"良かっ", "よかっ"},
                                 {"出来", "でき", true},
                                 {"沢山", "たくさん"},
                                 {"丁度", "ちょうど"},
                                 {"是非", "ぜひ"},
                                 {"流石", "さすが"},
                                 {"勿論", "もちろん"},
                                 {"宜しく", "よろしく"},
                                 {"有難う", "ありがとう"},
                                 {"頂き", "いただき", true},
                                 {"頂い", "いただい"},
                                 {"頂け", "いただけ"},
                                 {"中々", "なかなか"},
                                 {"又", "また"},
                                 {"為", "ため"},
                                 {"迄", "まで"},
                                 {"貰", "もら"},
                                 {"仕舞", "しま"},
                                 {"凄", "すご"},
                                 {"美味し", "おいし"},
                                 {"居る", "いる"},
                                 {"居た", "いた"},
                                 {"居て", "いて"},
                                 {"居な", "いな"},
                                 {"無い", "ない"},
                                 {"無く", "なく"},
                                 {"無かっ", "なかっ"},
                                 {"有る", "ある"},
                                 {"有っ", "あっ"},
                                 {"可愛", "かわい"},
                                 {"付い", "つい", true},
                                 {"綺麗", "きれい"},
                                 {"被", "かぶ", true},
                                 {"纏", "まと", true},
                                 {"掛け", "かけ", true},
                                 {"掛か", "かか", true},
                                 {"取り敢えず", "とりあえず"},
                                 {"大体", "だいたい"},
                                 {"殆ど", "ほとんど"},
                                 {"失くし", "なくし"},
                                 {"頂戴", "ちょうだい"},
                                 {"庇", "かば", true},
                                 {"騙", "だま", true},
                                 {"良く", "よく", false, true}};
    const auto hiragana = [](std::string_view rest) {
        if (rest.empty()) {
            return false;
        }
        const auto c = g_utf8_get_char(rest.data());
        return c >= 0x3041 && c <= 0x3096;
    };
    const auto replace_all = [](std::string& text, std::string_view from, std::string_view to,
                                auto&& fits) {
        for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
            if (fits(std::string_view(text).substr(0, at),
                     std::string_view(text).substr(at + from.size()))) {
                text.replace(at, from.size(), to);
                at += to.size();
            } else {
                at += from.size();
            }
        }
    };
    const auto any_end = [](std::string_view text, std::initializer_list<std::string_view> ends) {
        return std::any_of(ends.begin(), ends.end(), [&](auto end) { return text.ends_with(end); });
    };
    const auto any_start = [](std::string_view text, std::initializer_list<std::string_view> starts) {
        return std::any_of(starts.begin(), starts.end(), [&](auto s) { return text.starts_with(s); });
    };
    for (std::size_t index = 0; index < segments.size(); ++index) {
        auto& segment = segments[index];
        auto& candidates = segment.candidates;
        std::string text = candidates.front();
        // A view of the string itself, not of a temporary the conditional makes.
        const std::string_view previous =
            index > 0 ? std::string_view(segments[index - 1].candidates.front()) : std::string_view();
        // Mozc reads からかさ as 唐傘 and shows 傘: から is the particle.
        if (segment.reading.starts_with("からかさ") && text.starts_with("傘")) {
            candidates.insert(candidates.begin(), "から" + text);
            continue;
        }
        // And words chat writes in kanji where Mozc writes kana: 時 after a
        // clause (暇な時), ご飯, の方が, 置いた (not 〜ておいた), 曇り; but
        // うち read so, not 家.
        if (segment.reading.starts_with("うち") && text.starts_with("家")) {
            text.replace(0, std::string_view("家").size(), "うち");
        }
        for (const auto& [kana, kanji] :
             {std::pair{"ごはん", "ご飯"}, std::pair{"つかれ", "疲れ"}, std::pair{"いそがし", "忙し"},
              std::pair{"よふかし", "夜更かし"}, std::pair{"すべら", "滑ら"},
              std::pair{"すべり", "滑り"}, std::pair{"すべっ", "滑っ"}, std::pair{"すべる", "滑る"}}) {
            replace_all(text, kana, kanji, [](auto, auto) { return true; });
        }
        // ネタ that ends a clause is 寝た: やっと寝たから, 早く寝たけど.
        if (segment.reading.starts_with("ねた") && text.starts_with("ネタ") &&
            !(index > 0 && segments[index - 1].candidates.front().ends_with("の")) &&
            any_start(std::string_view(text).substr(std::string_view("ネタ").size()),
                      {"から", "けど", "のに", "ら"})) {
            text.replace(0, std::string_view("ネタ").size(), "寝た");
        }
        // Not とき alone.
        replace_all(text, "とき", "時", [&](std::string_view before, std::string_view rest) {
            return any_end(before.empty() ? previous : before, {"た", "な", "い", "る", "の"}) &&
                   (rest.empty() ||
                    any_start(rest, {"は", "に", "も", "の", "が", "から", "まで", "、", "で"}));
        });
        replace_all(text, "ほう", "方", [&](std::string_view before, std::string_view rest) {
            return any_end(before.empty() ? previous : before,
                           {"の", "た", "る", "な"}) && // Not ないほうが.
                   any_start(rest, {"が", "は", "も"});
        });
        const bool after_te = index > 0 && any_end(segments[index - 1].candidates.front(), {"て", "で"});
        if (!after_te &&
            any_start(segment.reading, {"おいた", "おいて", "おいと", "おいちゃ", "おきっぱ"}) &&
            text.starts_with("お")) {
            text.replace(0, std::string_view("お").size(), "置");
        }
        // おき after a time counter: 15分おき, 2日おき.
        if (segment.reading.starts_with("おき") &&
            (text.starts_with("起き") || text.starts_with("置き")) &&
            any_end(previous, {"分", "時間", "日", "週間", "年", "秒", "ヶ月", "か月", "駅"})) {
            text.replace(0, std::string_view("起き").size(), "おき");
        }
        // A word spelled out stays so: ごーるでんうぃーく is no GW.
        if (segment.reading.starts_with("ごーるでんうぃーく") && text.starts_with("GW")) {
            text.replace(0, 2, "ゴールデンウィーク");
        }
        if (segment.reading.starts_with("くもり") && text.starts_with("曇") &&
            !text.starts_with("曇り")) {
            text.insert(std::string_view("曇").size(), "り");
        }
        // 辛い is つらい where it is read so, not からい.
        if (segment.reading.find("つら") != std::string::npos) {
            for (auto at = text.find("辛"); at != std::string::npos; at = text.find("辛", at)) {
                text.replace(at, std::string_view("辛").size(), "つら");
            }
        }
        for (const auto& word : words) {
            for (auto at = text.find(word.kanji); at != std::string::npos;
                 at = text.find(word.kanji, at)) {
                const bool after_kanji =
                    at > 0 && g_unichar_get_script(g_utf8_get_char(g_utf8_find_prev_char(
                                  text.c_str(), text.c_str() + at))) == G_UNICODE_SCRIPT_HAN;
                if ((word.verb && !hiragana(std::string_view(text).substr(at + word.kanji.size()))) ||
                    (word.alone && after_kanji)) {
                    at += word.kanji.size();
                    continue;
                }
                text.replace(at, word.kanji.size(), word.kana);
                at += word.kana.size();
            }
        }
        const auto found = std::find(candidates.begin(), candidates.end(), text);
        if (text != candidates.front() && found != candidates.end()) {
            std::rotate(candidates.begin(), found, found + 1);
        }
    }
}
std::vector<JapaneseComposer::Segment> JapaneseComposer::fresh(const std::string& reading) {
    converted_.clear();
    converted_as_.clear();
    stretch_ = {};
    alternative_.clear();
    // A word stretched with ー, as chat writes it, converts as the word with
    // the ー kept: on its end (いいよー), or before a final い or う (ばいばーい).
    // Not where Anthy reads it whole as a katakana word (カレー, コーヒー).
    constexpr std::string_view long_vowel = "ー";
    // Numbers in kana are converted as digits (numbers_as_digits()).
    const auto spoken = numbers_as_digits(reading);
    auto base = spoken;
    Stretch stretch;
    while (std::string_view(base).ends_with(long_vowel) && g_utf8_strlen(base.c_str(), -1) > 2) {
        base.resize(base.size() - long_vowel.size());
        stretch.marks += long_vowel;
    }
    // Before another final kana only in a phrase the list has (いってきまーす,
    // いただきまーす), not in one it doesn't (まーね is no 真似).
    if (stretch.empty() && g_utf8_strlen(base.c_str(), -1) > 2) {
        const char* last = g_utf8_prev_char(base.c_str() + base.size());
        const std::string final(last);
        auto head = base.substr(0, base.size() - final.size());
        std::string marks;
        while (std::string_view(head).ends_with(long_vowel) && g_utf8_strlen(head.c_str(), -1) > 2) {
            head.resize(head.size() - long_vowel.size());
            marks += long_vowel;
        }
        if (!marks.empty() && hiragana_only(final) && final != long_vowel &&
            (final == "い" || final == "う" || (preferred_ && !preferred_(head + final).empty()))) {
            base = head + final;
            stretch = {marks, final};
        }
    }
    if (!stretch.empty()) {
        auto whole = imported(converter().convert(spoken));
        if (whole.size() == 1 && katakana_word(whole.front().candidates.front())) {
            converted_ = reading;
            resized_ = false;
            return whole;
        }
    }
    // Romaji's nn is ん, but ん before a vowel may have meant the n carried
    // over: minna reads みんあ for みんな. The reading Anthy splits into fewer
    // segments wins; on a tie the typed one, each segment the other reads
    // otherwise offering that second (三人 after 参院 for sannin; 原因 for
    // gennin, 千円 for sennen), and the other whole phrase among the live
    // candidates. Anthy resizes and learns its last conversion: the one used.
    // Numbers are found in the carried reading itself: nisennijuurokunen is
    // にせんにじゅうろくねん, 2026ねん, not にせんい16ねん.
    auto converted = romaji_ ? carried_n(base) : base;
    if (romaji_) {
        auto raw = reading;
        raw.resize(raw.size() - stretch.marks.size() - stretch.final.size());
        // Only where that finds more digits: sanpunnyonjuubyou stays 3ぷん40びょう.
        const auto digits = [](const std::string& text) {
            return std::count_if(text.begin(), text.end(),
                                 [](unsigned char c) { return c >= '0' && c <= '9'; });
        };
        if (auto numbered = numbers_as_digits(carried_n(raw + stretch.final));
            digits(numbered) > digits(base)) {
            converted = std::move(numbered);
        }
    }
    auto result = imported(converter().convert(converted));
    if (converted != base) {
        auto typed = imported(converter().convert(base));
        if (result.size() < typed.size() && carried_words(result, base)) {
            result = imported(converter().convert(converted));
        } else {
            if (result.size() == typed.size()) {
                std::string other, own;
                for (std::size_t i = 0; i < typed.size(); ++i) {
                    auto& candidates = typed[i].candidates;
                    const auto& text = result[i].candidates.front();
                    other += text;
                    own += candidates.front();
                    if (std::find(candidates.begin(), candidates.end(), text) == candidates.end()) {
                        candidates.insert(candidates.begin() + 1, text);
                    }
                }
                if (typed.size() > 1 && other != own) {
                    alternative_ = stretch.apply(other);
                }
            }
            result = std::move(typed);
            converted = base;
        }
    }
    numeral_digits(result);
    object_verbs(result);
    chat_kana(result);
    if (!stretch.empty()) {
        stretch_segment(result.back(), stretch);
    }
    if (converted != reading) {
        converted_as_ = std::move(converted);
        stretch_ = std::move(stretch);
    }
    converted_ = reading;
    resized_ = false;
    return result;
}
bool JapaneseComposer::carried_words(const std::vector<Segment>& segments,
                                     const std::string& typed) const {
    // Only where the carried n makes words: みんな, 何人で, ごめんね; not where
    // the typed reading was meant (じかんある, 時間なる; しけんいつ, 試験につ).
    const auto marks = carried_marks(typed);
    std::size_t at = 0;
    for (const auto& segment : segments) {
        const auto length = static_cast<std::size_t>(g_utf8_strlen(segment.reading.c_str(), -1));
        bool changed = false;
        for (std::size_t i = at; i < at + length && i < marks.size(); ++i) {
            changed = changed || marks[i];
        }
        const bool word =
            (words_ && words_->own_cost(segment.reading) != std::numeric_limits<int>::max()) ||
            (preferred_ && !preferred_(segment.reading).empty());
        if (changed && !word) {
            return false;
        }
        at += length;
    }
    return true;
}
void JapaneseComposer::stretch_segment(Segment& segment, const Stretch& stretch) {
    auto reading = stretch.apply(segment.reading);
    // A final kana split off on its own stays converted; don't move the
    // stretch onto another segment or discard this segment's candidates.
    if (reading.empty()) {
        return;
    }
    // Those that can't take it go: 倍々 for ばいばい, as ばいばーい.
    std::vector<std::string> candidates;
    for (const auto& candidate : segment.candidates) {
        if (auto stretched = stretch.apply(candidate);
            !stretched.empty() &&
            std::find(candidates.begin(), candidates.end(), stretched) == candidates.end()) {
            candidates.push_back(std::move(stretched));
        }
    }
    if (!candidates.empty()) {
        segment.reading = std::move(reading);
        segment.candidates = std::move(candidates);
    }
}
std::vector<std::string> JapaneseComposer::usual_phrases(const std::string& reading) const {
    auto phrases = preferred_ ? preferred_(reading) : std::vector<std::string>{};
    if (!phrases.empty()) {
        return phrases;
    }
    if (preferred_ && !converted_as_.empty() && converted_ == reading) {
        for (const auto& phrase : preferred_(converted_as_)) {
            if (auto stretched = stretch_.apply(phrase); !stretched.empty()) {
                phrases.push_back(std::move(stretched));
            }
        }
        // An adjective chat stretches in place of its い, as typed, then
        // its stem: かわいー, then 可愛ー.
        if (phrases.empty() && words_ && !stretch_.empty() && stretch_.final.empty()) {
            const auto adjective = words_->exact(converted_as_ + "い", 1);
            if (!adjective.empty() && std::string_view(adjective.front()).ends_with("い")) {
                phrases = {reading, adjective.front().substr(0, adjective.front().size() -
                                                                    std::string_view("い").size()) +
                                        stretch_.marks};
            }
        }
    }
    // A loanword typed in kana, as the word list writes it: こんびに as コンビニ,
    // でーと デート; not a stretched word (やったー).
    if (phrases.empty() && words_ && stretch_.empty() && loanword_length(reading)) {
        if (auto words = words_->exact(reading, 1); !words.empty() && katakana_word(words.front())) {
            phrases = std::move(words);
        }
    }
    return phrases;
}
void JapaneseComposer::append(const std::string& text) {
    // Inserted at the cursor; a combining mark joins the kana before it.
    const auto before = normalized(reading_.substr(0, cursor_) + text);
    reading_ = before + reading_.substr(cursor_);
    cursor_ = before.size();
}
std::string JapaneseComposer::before_cursor() const {
    if (cursor_ == 0) {
        return {};
    }
    const auto* at = g_utf8_find_prev_char(reading_.c_str(), reading_.c_str() + cursor_);
    return std::string(at, static_cast<std::size_t>(reading_.c_str() + cursor_ - at));
}
void JapaneseComposer::erase_before_cursor() {
    const auto size = before_cursor().size();
    reading_.erase(cursor_ - size, size);
    cursor_ -= size;
}
bool JapaneseComposer::move(int direction) {
    if (converting() || (direction != 1 && direction != -1)) {
        return false;
    }
    drain(true); // Romaji still waiting settles where it was typed.
    if (direction < 0) {
        if (cursor_ == 0) {
            return false;
        }
        cursor_ -= before_cursor().size();
    } else {
        if (cursor_ >= reading_.size()) {
            return false;
        }
        cursor_ =
            static_cast<std::size_t>(g_utf8_next_char(reading_.c_str() + cursor_) - reading_.c_str());
    }
    return true;
}
std::size_t JapaneseComposer::cursor() const {
    return cursor_ + pending_.size();
}
void JapaneseComposer::drain(bool finish) {
    const auto& table = roman_table();
    while (!pending_.empty()) {
        if (pending_[0] == 'n') {
            if (pending_.size() == 1) {
                if (finish) {
                    append("ん");
                    pending_.clear();
                }
                return;
            }
            const char next = pending_[1];
            if (next == '\'') {
                append("ん");
                pending_.erase(0, 2);
                continue;
            }
            if (next == 'n') {
                // nn is one ん, as in Mozc's table and other IMEs: honnya
                // is ほんや. A reading spelled so also offers the word the
                // other reading makes (こんいちは: こんにちは; prepare_live).
                append("ん");
                pending_.erase(0, 2);
                continue;
            }
            if (std::string("aiueoy").find(next) == std::string::npos) {
                append("ん");
                pending_.erase(0, 1);
                continue;
            }
        }
        if (pending_.size() > 1 && pending_[0] == pending_[1] &&
            std::string("bcdfghjklmpqrstvwxyz").find(pending_[0]) != std::string::npos) {
            append("っ");
            pending_.erase(0, 1);
            continue;
        }
        if (auto it = table.find(pending_); it != table.end()) {
            append(it->second);
            pending_.clear();
            return;
        }
        const bool prefix = std::any_of(table.begin(), table.end(), [&](const auto& pair) {
            return pair.first.starts_with(pending_);
        });
        if (prefix && !finish) {
            return;
        }
        append(pending_.substr(0, 1));
        pending_.erase(0, 1);
    }
}
void JapaneseComposer::roman(char key) {
    if (g_utf8_strlen(reading_.c_str(), -1) >= 30) {
        throw std::runtime_error("Commit this phrase before typing more");
    }
    unconvert();
    romaji_ = true;
    pending_ += static_cast<char>(g_ascii_tolower(key));
    drain(false);
}
void JapaneseComposer::kana(const std::string& text) {
    if (g_utf8_strlen(reading_.c_str(), -1) >= 30) {
        throw std::runtime_error("Commit this phrase before typing more");
    }
    unconvert();
    drain(true);
    append(text);
}
namespace {
// Each kana is three UTF-8 bytes; the strings pair small and large forms,
// hiragana then katakana (the reading may be in either after カタカナ/F7).
constexpr std::string_view small_kana = "ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮ",
                           large_kana = "あいうえおつやゆよわアイウエオツヤユヨワ";
std::string small_form(const std::string& kana) {
    const auto at = large_kana.find(kana);
    return kana.empty() || at == std::string_view::npos ? std::string{}
                                                        : std::string(small_kana.substr(at, 3));
}
std::string large_form(const std::string& kana) {
    const auto at = small_kana.find(kana);
    return kana.empty() || at == std::string_view::npos ? kana : std::string(large_kana.substr(at, 3));
}
// Base kana plus a combining mark, if Unicode composes them into one character.
std::string marked(const std::string& base, const char* combining) {
    const auto text = normalized(base + combining);
    return g_utf8_strlen(text.c_str(), -1) == 1 ? text : std::string{};
}
} // namespace
std::string JapaneseComposer::last_kana() const {
    if (!pending_.empty() || converting()) {
        return {};
    }
    return before_cursor();
}
bool JapaneseComposer::mark(const std::string& mark) {
    drain(true);
    // The kana before the cursor, also while converting: the conversion is
    // only given up below if the mark actually changes it.
    const auto last = before_cursor();
    if (last.empty()) {
        return false;
    }
    // The plain, large form: が -> か, っ -> つ.
    auto* decomposed = g_utf8_normalize(last.c_str(), -1, G_NORMALIZE_DEFAULT);
    const auto base = large_form(
        std::string(decomposed, static_cast<std::size_t>(g_utf8_next_char(decomposed) - decomposed)));
    g_free(decomposed);
    const auto small = small_form(base), voiced = marked(base, "\u3099"), semi = marked(base, "\u309a");
    std::string next;
    if (mark == "゛" && !voiced.empty()) {
        next = last == voiced ? base : voiced;
    } else if (mark == "゜" && !semi.empty()) {
        next = last == semi ? base : semi;
    } else if (mark == "小" && !small.empty()) {
        next = last == small ? base : small;
    } else if (mark == "゛゜小") {
        // Phones cycle base, small, voiced, semi-voiced: つ っ づ, は ば ぱ.
        std::vector<std::string> variants{base};
        for (const auto& variant : {small, voiced, semi}) {
            if (!variant.empty()) {
                variants.push_back(variant);
            }
        }
        const auto at = std::find(variants.begin(), variants.end(), last);
        next = at == variants.end() || at + 1 == variants.end() ? variants.front() : *(at + 1);
    }
    if (next.empty() || next == last) {
        return false;
    }
    unconvert();
    erase_before_cursor();
    reading_.insert(cursor_, next);
    cursor_ += next.size();
    return true;
}
void JapaneseComposer::backspace() {
    if (converting()) {
        unconvert();
        return;
    }
    if (!pending_.empty()) {
        pending_.pop_back();
    } else {
        erase_before_cursor();
    }
    if (empty()) {
        live_ = {}; // Nothing left to stand for.
    }
}
void JapaneseComposer::convert() {
    convert(true);
}
void JapaneseComposer::convert(bool preferring) {
    if (converting()) {
        cycle(1);
        return;
    }
    drain(true);
    if (empty()) {
        return;
    }
    // Live candidates converted this reading already, and nothing since.
    segments_ = live_.reading == reading_ && converted_ == reading_ && !resized_ ? live_.segments
                                                                                 : fresh(reading_);
    active_ = 0;
    phrase_.reset();
    // Also the reading as converted, the n carried over, a ー kept on.
    if (const auto phrases =
            preferring && preferred_ ? usual_phrases(reading_) : std::vector<std::string>{};
        !phrases.empty()) {
        if (segments_.size() == 1) {
            auto& candidates = segments_.front().candidates;
            for (const auto& phrase : phrases) {
                std::erase(candidates, phrase);
            }
            candidates.insert(candidates.begin(), phrases.begin(), phrases.end());
        } else {
            // One segment for the phrase, the converter's own split and the
            // scripts after it. Resizing goes back to the converter's split;
            // the converter learns nothing from a phrase it didn't split so.
            std::string whole;
            for (const auto& segment : segments_) {
                whole += segment.candidates.front();
            }
            Segment phrase{reading_, {}};
            auto texts = phrases;
            texts.insert(texts.end(), {whole, in_script(reading_, false), in_script(reading_, true)});
            for (auto& text : texts) {
                if (std::find(phrase.candidates.begin(), phrase.candidates.end(), text) ==
                    phrase.candidates.end()) {
                    phrase.candidates.push_back(std::move(text));
                }
            }
            segments_ = {std::move(phrase)};
            phrase_ = Phrase{segments_.front(), {}, 0};
        }
    }
}
void JapaneseComposer::cycle(int direction) {
    if (!converting()) {
        return;
    }
    auto& s = segments_.at(static_cast<std::size_t>(active_));
    const int count = static_cast<int>(s.candidates.size());
    s.selected = (s.selected + direction + count) % count;
}
void JapaneseComposer::segment(int direction) {
    if (converting()) {
        active_ = std::clamp(active_ + direction, 0, segment_count() - 1);
    }
}
void JapaneseComposer::resize(int direction) {
    if (!converting() || (direction != 1 && direction != -1)) {
        return;
    }
    if (phrase_ && active_ == 0) {
        if (resize_phrase(direction)) {
            return;
        }
        phrase_.reset(); // Its text doesn't end so: the converter's split.
    }
    const auto& active = segments_[static_cast<std::size_t>(active_)];
    // At least one character, and no further than the end of the reading.
    if ((direction < 0 && g_utf8_strlen(active.reading.c_str(), -1) <= 1) ||
        (direction > 0 && active_ + 1 >= segment_count())) {
        return;
    }
    if (converted_ != reading_) {
        // Something else was converted since: Anthy resizes only its last
        // conversion, so convert this again if that splits it the same way.
        const auto again = fresh(reading_);
        if (!std::equal(again.begin(), again.end(), segments_.begin(), segments_.end(),
                        [](const Segment& a, const Segment& b) { return a.reading == b.reading; })) {
            converted_.clear(); // The boundaries differ; don't guess.
            return;
        }
    }
    auto resized = imported(converter().resize(active_, direction));
    resized_ = true;
    if (resized.empty() || static_cast<int>(resized.size()) <= active_) {
        return;
    }
    // The segments before the active one keep their choices.
    for (int i = 0; i < active_; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (resized[index].reading == segments_[index].reading) {
            resized[index] = segments_[index];
        }
    }
    numeral_digits(resized);
    object_verbs(resized);
    chat_kana(resized);
    if (!stretch_.empty()) {
        stretch_segment(resized.back(), stretch_); // Anthy converted it without.
    }
    // Rules may reorder a kept segment's candidates when its neighbour
    // changes. Its chosen text still belongs to the user.
    for (int i = 0; i < active_; ++i) {
        const auto index = static_cast<std::size_t>(i);
        auto& segment = resized[index];
        const auto& before = segments_[index];
        if (segment.reading == before.reading) {
            const auto& chosen = before.candidates[static_cast<std::size_t>(before.selected)];
            const auto found = std::find(segment.candidates.begin(), segment.candidates.end(), chosen);
            if (found != segment.candidates.end()) {
                segment.selected = static_cast<int>(found - segment.candidates.begin());
            }
        }
    }
    segments_ = std::move(resized);
}
bool JapaneseComposer::choose(int index) {
    if (!converting() || index < 0 ||
        index >= static_cast<int>(segments_[static_cast<std::size_t>(active_)].candidates.size())) {
        return false;
    }
    segments_[static_cast<std::size_t>(active_)].selected = index;
    if (active_ + 1 >= segment_count()) {
        return true;
    }
    ++active_;
    return false;
}
bool JapaneseComposer::settled(std::string& reading) const {
    // A lone n would become ん; any other letter could still change.
    if (!pending_.empty() && pending_ != "n") {
        return false;
    }
    reading = pending_.empty()
                  ? reading_
                  : normalized(reading_.substr(0, cursor_) + "ん") + reading_.substr(cursor_);
    return true;
}
bool JapaneseComposer::live_current() const {
    std::string reading;
    return !converting() && !live_.reading.empty() && settled(reading) && reading == live_.reading;
}
void JapaneseComposer::prepare_live() {
    std::string reading;
    if (converting() || !settled(reading) || reading.empty() || reading == live_.reading) {
        return;
    }
    Live live;
    live.segments = fresh(reading);
    live.reading = reading;
    auto add = [&](std::string text, Live::Source source, int index = 0) {
        if (!text.empty() && std::find(live.texts.begin(), live.texts.end(), text) == live.texts.end()) {
            live.texts.push_back(std::move(text));
            live.sources.emplace_back(source, index);
        }
    };
    std::string whole;
    for (const auto& segment : live.segments) {
        whole += segment.candidates.front();
    }
    // A reading that is no word yet, or a rarer one than what it begins,
    // completes first, as on a phone: おめでと to おめでとう.
    bool completes_first = false;
    std::vector<std::pair<std::string, int>> completed;
    const bool long_enough = g_utf8_strlen(reading.c_str(), -1) >= 2;
    if (words_ && long_enough) {
        completed = words_->completions(reading, 3, completes_first);
    }
    // Common chat phrases complete it too, ranked as fairly common words:
    // いただき to いただきます, ひさし to 久しぶり, but きょうは and こんや to 今日は
    // and 今夜 first. A word the phrases list too ranks well ahead, as
    // chatted: こんに to こんにちは before 今日 (こんにち).
    if (phrases_ && long_enough) {
        constexpr int phrase_cost = 3000, chatted = 1000;
        bool ignored = false;
        std::set<std::string> phrases_only;
        for (auto& [text, cost] : phrases_->completions(reading, 3, ignored)) {
            const auto same = std::find_if(completed.begin(), completed.end(),
                                           [&](const auto& got) { return got.first == text; });
            if (same != completed.end()) {
                same->second -= chatted;
            } else {
                phrases_only.insert(text);
                completed.emplace_back(std::move(text), phrase_cost);
            }
        }
        std::stable_sort(completed.begin(), completed.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; });
        if (completed.size() > 3) {
            completed.resize(3);
        }
        // A phrase only the list has doesn't lead a word and a particle:
        // くるまで is 車で before 車で来た.
        const bool over_particle = !completed.empty() &&
                                   phrases_only.contains(completed.front().first) && words_ &&
                                   words_->word_and_particle(reading);
        completes_first = completes_first ||
                          (!completed.empty() && !over_particle &&
                           (!words_ || words_->completion_leads(reading, completed.front().second)));
    }
    // A reading the list lacks that the converter reads whole as a word, as
    // Mozc's larger dictionary does (法事, 薄着, 年始), counts as a word rarer
    // than any listed: only a clearly commoner completion leads it.
    // Two kana are too few to tell: すご completes to すごい.
    if (completes_first && words_ && !completed.empty() && live.segments.size() == 1 &&
        g_utf8_strlen(reading.c_str(), -1) > 2 &&
        words_->own_cost(reading) == std::numeric_limits<int>::max() && has_kanji(whole)) {
        constexpr int unlisted = 5500;
        completes_first = words_->completion_leads(reading, completed.front().second, unlisted);
    }
    auto complete = [&] {
        for (auto& [word, cost] : completed) {
            add(std::move(word), Live::Source::Completion);
        }
        completed.clear();
    };
    // A reading typed in katakana is wanted as typed (ガッ).
    if (reading == in_script(reading, true) && reading != in_script(reading, false)) {
        add(reading, Live::Source::Katakana);
    }
    // The usual conversion of a common phrase comes before all of them,
    // after the user's own choice for it.
    if (preferred_) {
        for (auto& phrase : usual_phrases(reading)) {
            add(std::move(phrase), Live::Source::Completion);
        }
    }
    const auto predicted = learning_ ? converter().predict(reading) : std::vector<std::string>{};
    // A conversion committed before, offered back as learned, is the user's
    // pick for the reading: it leads the dictionary's completions.
    if (std::find(predicted.begin(), predicted.end(), whole) != predicted.end()) {
        completes_first = false;
    }
    // Nor before a stretched word or a katakana word Anthy reads whole:
    // だめー is ダメー before ダメージ, こーら コーラ before コーラス.
    if (!stretch_.empty() ||
        (live.segments.size() == 1 && katakana_word(whole) && loanword_length(reading))) {
        completes_first = false;
    }
    if (completes_first) {
        complete();
    }
    add(whole, Live::Source::Whole);
    // On a tie between romaji's readings, the other one's phrase next.
    add(alternative_, Live::Source::Completion);
    for (std::size_t i = 0; i < predicted.size(); ++i) {
        add(predicted[i], Live::Source::Prediction, static_cast<int>(i));
    }
    // Romaji's nn is ん, so konnichiha reads こんいちは: the words the reading
    // makes with the n carried over (こんにちは) come first while it makes
    // none itself, and after its conversion otherwise (原因, then 現任).
    std::vector<std::string> carried;
    if (romaji_) {
        if (const auto other = carried_n(reading); other != reading) {
            if (preferred_) {
                carried = preferred_(other);
            }
            if (words_) {
                for (auto& word : words_->exact(other, 2)) {
                    carried.push_back(std::move(word));
                }
            }
        }
    }
    // The reading is a word too when Anthy converts it whole to one (千円 for
    // せんえん, before 専念 for せんねん).
    const auto& own = live.segments.front().candidates.front();
    const bool anthy_word =
        live.segments.size() == 1 && own != in_script(reading, false) && own != in_script(reading, true);
    const bool carried_first =
        !carried.empty() && !anthy_word &&
        (!words_ || words_->own_cost(reading) == std::numeric_limits<int>::max()) &&
        (!preferred_ || preferred_(reading).empty());
    if (carried_first) {
        std::vector<std::string> texts;
        std::vector<std::pair<Live::Source, int>> sources;
        for (auto& text : carried) {
            if (std::find(texts.begin(), texts.end(), text) == texts.end()) {
                texts.push_back(std::move(text));
                sources.emplace_back(Live::Source::Completion, 0);
            }
        }
        carried.clear();
        // Before everything listed so far.
        for (std::size_t i = 0; i < live.texts.size(); ++i) {
            if (std::find(texts.begin(), texts.end(), live.texts[i]) == texts.end()) {
                texts.push_back(live.texts[i]);
                sources.push_back(live.sources[i]);
            }
        }
        live.texts = std::move(texts);
        live.sources = std::move(sources);
    }
    // A reading that is one word offers its likeliest other words before
    // completing it, as completions that lead have already: 雨 and 飴 before
    // 編める, 神, 紙 and 髪 before 噛み合う.
    const auto& first = live.segments.front().candidates;
    constexpr std::size_t before_completions = 5;
    const std::size_t early = live.segments.size() == 1 ? std::min(first.size(), before_completions) : 0;
    for (std::size_t i = 0; i < early; ++i) {
        add(first[i], Live::Source::First, static_cast<int>(i));
    }
    for (auto& text : carried) {
        add(std::move(text), Live::Source::Completion);
    }
    complete();
    for (std::size_t i = early; i < first.size(); ++i) {
        add(first[i], Live::Source::First, static_cast<int>(i));
    }
    add(in_script(reading, false), Live::Source::Hiragana);
    add(in_script(reading, true), Live::Source::Katakana);
    if (emoji_) {
        // For the reading and its likeliest conversions: ねこ and 猫 find 🐈.
        std::vector<std::string> words{in_script(reading, false), whole};
        for (std::size_t i = 0; i < std::min<std::size_t>(first.size(), 3); ++i) {
            words.push_back(first[i]);
        }
        for (auto& text : emoji_(words)) {
            add(std::move(text), Live::Source::Emoji);
        }
    }
    if (kaomoji_) {
        // Every one for the reading, picked like an emoji in place of it.
        for (auto& face : kaomoji_->exact(in_script(reading, false), 1000)) {
            add(std::move(face), Live::Source::Emoji);
        }
    }
    live_ = std::move(live);
}
bool JapaneseComposer::choose_live(int index) {
    if (!live_current() || index < 0 || index >= static_cast<int>(live_.texts.size())) {
        return false;
    }
    drain(true); // A pending n is the ん the list was converted with.
    const auto [source, at] = live_.sources[static_cast<std::size_t>(index)];
    if (source == Live::Source::Emoji) {
        emoji_choice_ = EmojiChoice{live_.texts[static_cast<std::size_t>(index)], reading_};
        reading_ = emoji_choice_->emoji; // In place of the whole reading.
        cursor_ = reading_.size();
        return true;
    }
    if (source == Live::Source::Prediction || source == Live::Source::Completion) {
        // A learned or common word stands for the whole reading, as typed so far.
        predicted_ = live_.texts[static_cast<std::size_t>(index)];
        reading_ = *predicted_;
        cursor_ = reading_.size();
        return true;
    }
    if (source == Live::Source::Hiragana || source == Live::Source::Katakana) {
        reading_ = in_script(reading_, source == Live::Source::Katakana);
        cursor_ = reading_.size();
        return true;
    }
    convert(false); // The converter's own split, which the chip shows.
    if (source == Live::Source::Whole) {
        return true;
    }
    // A first segment's candidate, found by its text should the conversion differ.
    const auto& candidates = segments_.front().candidates;
    const auto& text = live_.texts[static_cast<std::size_t>(index)];
    const auto found = std::find(candidates.begin(), candidates.end(), text);
    return choose(found != candidates.end() ? static_cast<int>(found - candidates.begin()) : at);
}
void JapaneseComposer::script(bool katakana) {
    drain(true);
    unconvert();
    const auto before = in_script(reading_.substr(0, cursor_), katakana);
    reading_ = before + in_script(reading_.substr(cursor_), katakana);
    cursor_ = before.size();
}
void JapaneseComposer::unconvert() {
    segments_.clear();
    active_ = 0;
    phrase_.reset();
}
bool JapaneseComposer::resize_phrase(int direction) {
    auto& phrase = *phrase_;
    const auto& reading = phrase.whole.reading;
    const int length = static_cast<int>(g_utf8_strlen(reading.c_str(), -1));
    const int cut = phrase.cut - direction; // ← cuts one more kana off.
    if (cut < 0 || cut >= length) {
        return true; // Whole again, or one kana left: nothing further.
    }
    if (phrase.cut == 0) {
        const auto& shown = segments_.front();
        phrase.text = shown.candidates[static_cast<std::size_t>(shown.selected)];
    }
    if (cut == 0) {
        segments_ = {phrase.whole};
        phrase.cut = 0;
        active_ = 0;
        return true;
    }
    // The kana cut off end the phrase as written too (今日はいい天気だね), so
    // the rest keeps its text (今日はいい天気だ) and they convert apart.
    const auto split = static_cast<std::size_t>(g_utf8_offset_to_pointer(reading.c_str(), length - cut) -
                                                reading.c_str());
    const auto head = reading.substr(0, split), tail = reading.substr(split);
    if (!std::string_view(phrase.text).ends_with(tail) || phrase.text.size() == tail.size()) {
        return phrase.cut > 0; // Cut as far as it goes, or not at all.
    }
    Segment first{head, {phrase.text.substr(0, phrase.text.size() - tail.size())}};
    for (auto script : {in_script(head, false), in_script(head, true)}) {
        if (std::find(first.candidates.begin(), first.candidates.end(), script) ==
            first.candidates.end()) {
            first.candidates.push_back(std::move(script));
        }
    }
    // The kana cut off stay as written first, as one segment, with the
    // converter's conversions of them after: ね, not 根.
    Segment last{tail, {tail}};
    const auto offer = [&](std::string text) {
        if (!text.empty() &&
            std::find(last.candidates.begin(), last.candidates.end(), text) == last.candidates.end()) {
            last.candidates.push_back(std::move(text));
        }
    };
    const auto converted = imported(converter().convert(tail));
    std::string joined;
    for (const auto& segment : converted) {
        joined += segment.candidates.front();
    }
    offer(joined);
    if (converted.size() == 1) {
        for (const auto& text : converted.front().candidates) {
            offer(text);
        }
    }
    offer(in_script(tail, true));
    converted_ = tail;
    resized_ = true;
    segments_ = {std::move(first), std::move(last)};
    phrase.cut = cut;
    active_ = 0;
    return true;
}
void JapaneseComposer::cancel() {
    unconvert();
    reading_.clear();
    pending_.clear();
    romaji_ = false;
    cursor_ = 0;
    live_ = {};
    predicted_.reset();
    emoji_choice_.reset();
}
std::string JapaneseComposer::reopen_reading() const {
    if (predicted_ && !live_.reading.empty()) {
        return live_.reading;
    }
    // Only while the emoji is still all there is: typing after a refused
    // commit makes it part of the reading.
    return emoji_choice_ && reading_ == emoji_choice_->emoji && pending_.empty() && !converting()
               ? emoji_choice_->reading
               : reading_;
}
std::string JapaneseComposer::typed_reading() const {
    return reopen_reading();
}
std::string JapaneseComposer::preedit(bool mark_active) const {
    if (!converting()) {
        // Romaji still waiting shows where it was typed, at the cursor.
        return reading_.substr(0, cursor_) + pending_ + reading_.substr(cursor_);
    }
    std::string result;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& segment = segments_[i];
        const bool selected = mark_active && static_cast<int>(i) == active_;
        if (selected) {
            result += "【";
        }
        result += segment.candidates[static_cast<std::size_t>(segment.selected)];
        if (selected) {
            result += "】";
        }
    }
    return result;
}
std::pair<std::size_t, std::size_t> JapaneseComposer::active_range() const {
    if (!converting()) {
        return {};
    }
    std::size_t begin = 0;
    for (int i = 0; i < active_; ++i) {
        const auto& segment = segments_[static_cast<std::size_t>(i)];
        begin += segment.candidates[static_cast<std::size_t>(segment.selected)].size();
    }
    const auto& active = segments_[static_cast<std::size_t>(active_)];
    return {begin, begin + active.candidates[static_cast<std::size_t>(active.selected)].size()};
}
std::string JapaneseComposer::commit_text() {
    drain(true);
    return preedit();
}
void JapaneseComposer::learn() {
    if (!learning_) {
        return;
    }
    if (predicted_) {
        converter().learn_prediction(live_.reading, *predicted_);
    } else if (converting() && converted_ == reading_) {
        // The converter's last conversion is this phrase, as resized here.
        std::vector<std::string> chosen;
        for (const auto& segment : segments_) {
            chosen.push_back(segment.candidates[static_cast<std::size_t>(segment.selected)]);
        }
        converter().learn(chosen);
    }
}
std::vector<std::string> JapaneseComposer::candidates() const {
    return converting() ? segments_[static_cast<std::size_t>(active_)].candidates
                        : std::vector<std::string>{};
}
int JapaneseComposer::selected() const {
    return converting() ? segments_[static_cast<std::size_t>(active_)].selected : 0;
}
void JapaneseWords::load() const {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    if (file_.empty()) {
        return;
    }
    std::ifstream input(file_, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    text_ = std::move(content).str();
    // reading<TAB>word<TAB>cost lines, sorted by reading; # starts a comment.
    std::size_t at = 0;
    while (at < text_.size()) {
        auto end = text_.find('\n', at);
        if (end == std::string::npos) {
            end = text_.size();
        }
        const std::string_view line(text_.data() + at, end - at);
        const auto first = line.find('\t'), second = line.find('\t', first + 1);
        if (!line.empty() && line[0] != '#' && first != std::string_view::npos &&
            second != std::string_view::npos && first > 0 && second > first + 1 &&
            second - first < 256) {
            Entry entry;
            entry.reading = static_cast<std::uint32_t>(at);
            entry.reading_size = static_cast<std::uint16_t>(std::min<std::size_t>(first, 255));
            entry.word = static_cast<std::uint32_t>(at + first + 1);
            entry.word_size = static_cast<std::uint16_t>(second - first - 1);
            int cost = 0;
            for (const char c : line.substr(second + 1)) {
                if (c < '0' || c > '9' || cost > 6000) {
                    break;
                }
                cost = cost * 10 + (c - '0');
            }
            entry.cost = static_cast<std::uint16_t>(std::min(cost, 65535));
            entries_.push_back(entry);
        }
        at = end + 1;
    }
}
// How much lower a completion's cost must be than the reading's own as a
// word to lead it.
constexpr int clearly_commoner = 1000;
bool JapaneseWords::word_and_particle(const std::string& reading) const {
    return exact(reading, 1).empty() && own_cost(reading) != std::numeric_limits<int>::max();
}
int JapaneseWords::own_cost(const std::string& reading) const {
    load();
    const auto reading_of = [&](const Entry& entry) {
        return std::string_view(text_.data() + entry.reading, entry.reading_size);
    };
    const auto cost_of = [&](std::string_view word) {
        int best = std::numeric_limits<int>::max();
        for (auto at = std::lower_bound(
                 entries_.begin(), entries_.end(), word,
                 [&](const Entry& entry, std::string_view key) { return reading_of(entry) < key; });
             at != entries_.end() && reading_of(*at) == word; ++at) {
            best = std::min(best, static_cast<int>(at->cost));
        }
        return best;
    };
    if (const int cost = cost_of(reading); cost != std::numeric_limits<int>::max()) {
        return cost;
    }
    // A word and a particle after it reads as the word, though less surely:
    // きょうは as 今日は before 脅迫 (きょうはく), but ありが to ありがとう.
    constexpr int particle_penalty = 2000;
    int cost = std::numeric_limits<int>::max();
    for (const std::string_view particle :
         {"は", "が", "を", "に", "で", "と", "も", "の", "へ", "や", "か", "ね", "よ"}) {
        if (reading.size() > particle.size() && std::string_view(reading).ends_with(particle)) {
            const int word = cost_of(std::string_view(reading.data(), reading.size() - particle.size()));
            if (word != std::numeric_limits<int>::max()) {
                cost = std::min(cost, word + particle_penalty);
            }
        }
    }
    return cost;
}
bool JapaneseWords::completion_leads(const std::string& reading, int cost, int unlisted) const {
    // A word read so leads its completions unless they are clearly commoner:
    // おめでとう (1842) leads おめでと (4192), but 涙 keeps its place before
    // 涙する and だめ before ダメージ. A word of two kana always leads, as a
    // phone's does: はな is 花 before 話 (はなし).
    // A reading ending in った or んだ is a verb's past, as typed: あった
    // before あったかい, やった before ヤッター, しんだ (死んだ) before 診断.
    if (std::string_view(reading).ends_with("った") || std::string_view(reading).ends_with("んだ")) {
        return false;
    }
    int own = own_cost(reading);
    if (own == std::numeric_limits<int>::max()) {
        if (unlisted == std::numeric_limits<int>::max()) {
            return true;
        }
        own = unlisted;
    }
    return g_utf8_strlen(reading.c_str(), -1) > 2 && cost + clearly_commoner < own;
}
std::vector<std::pair<std::string, int>>
JapaneseWords::completions(const std::string& reading, std::size_t most, bool& first) const {
    load();
    int exact = std::numeric_limits<int>::max(); // The reading's own cost as a word.
    const auto reading_of = [&](const Entry& entry) {
        return std::string_view(text_.data() + entry.reading, entry.reading_size);
    };
    // The entries whose reading begins with this one follow each other.
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), std::string_view(reading),
        [&](const Entry& entry, std::string_view key) { return reading_of(entry) < key; });
    // A small kana right after the reading would make its last kana another
    // sound: きみ is no start of きみょう.
    constexpr std::string_view small = "ゃゅょぁぃぅぇぉゎ";
    std::vector<const Entry*> found;
    for (; it != entries_.end() && reading_of(*it).starts_with(reading); ++it) {
        const auto rest = reading_of(*it).substr(reading.size());
        if (rest.empty()) {
            exact = std::min(exact, static_cast<int>(it->cost));
        } else if (rest.size() < 3 || small.find(rest.substr(0, 3)) == std::string_view::npos) {
            found.push_back(&*it);
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Entry* a, const Entry* b) { return a->cost < b->cost; });
    // An adjective's stem completes to it first unless the stem is clearly
    // the commoner word: おもしろ to 面白い. Only an adjective: 協会 is no
    // stem's completion of きょうか, nor 見解 of けんか.
    const bool stem =
        !found.empty() && reading_of(*found.front()) == reading + "い" &&
        std::string_view(text_.data() + found.front()->word, found.front()->word_size).ends_with("い") &&
        exact != std::numeric_limits<int>::max() && g_utf8_strlen(reading.c_str(), -1) > 2 &&
        exact + clearly_commoner >= static_cast<int>(found.front()->cost);
    first = !found.empty() && (stem || completion_leads(reading, static_cast<int>(found.front()->cost)));
    std::vector<std::pair<std::string, int>> result;
    for (const auto* entry : found) {
        std::string word(text_.data() + entry->word, entry->word_size);
        if (std::none_of(result.begin(), result.end(),
                         [&](const auto& got) { return got.first == word; })) {
            result.emplace_back(std::move(word), static_cast<int>(entry->cost));
            if (result.size() == most) {
                break;
            }
        }
    }
    return result;
}
std::vector<std::string> JapaneseWords::complete(const std::string& reading, std::size_t most,
                                                 bool& first) const {
    std::vector<std::string> result;
    for (auto& [word, cost] : completions(reading, most, first)) {
        result.push_back(std::move(word));
    }
    return result;
}
std::vector<std::string> JapaneseWords::exact(const std::string& reading, std::size_t most) const {
    load();
    const auto reading_of = [&](const Entry& entry) {
        return std::string_view(text_.data() + entry.reading, entry.reading_size);
    };
    const auto [from, to] = std::equal_range(
        entries_.begin(), entries_.end(), std::string_view(reading), [&](const auto& a, const auto& b) {
            if constexpr (std::is_same_v<std::decay_t<decltype(a)>, Entry>) {
                return reading_of(a) < b;
            } else {
                return a < reading_of(b);
            }
        });
    std::vector<const Entry*> found;
    for (auto it = from; it != to; ++it) {
        found.push_back(&*it);
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Entry* a, const Entry* b) { return a->cost < b->cost; });
    std::vector<std::string> result;
    for (std::size_t i = 0; i < found.size() && result.size() < most; ++i) {
        result.emplace_back(text_.data() + found[i]->word, found[i]->word_size);
    }
    return result;
}
std::string kana_in_script(const std::string& text, bool katakana) {
    return in_script(text, katakana);
}
} // namespace framekeyboard

#include "framekeyboard/japanese.hpp"
#include <algorithm>
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
// Punctuation and symbols alone, such as ！, 」 or …, with no kana.
bool symbols_only(const std::string& reading) {
    for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
        if (!g_unichar_ispunct(g_utf8_get_char(p))) {
            return false;
        }
    }
    return !reading.empty();
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
JapaneseComposer::JapaneseComposer(Factory factory) : factory_(std::move(factory)) {
}
JapaneseComposer::~JapaneseComposer() = default;
JapaneseConverter& JapaneseComposer::converter() {
    if (!converter_) {
        converter_ = factory_ ? factory_() : std::make_unique<AnthyConverter>();
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
                    (!std::string_view(text).ends_with(particle) && common_word(reading, text))) {
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
        auto forms = usual_forms(segment.reading, segment.candidates.front(), place);
        if (forms.empty()) {
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
std::vector<JapaneseComposer::Segment> JapaneseComposer::fresh(const std::string& reading) {
    converted_.clear();
    converted_as_.clear();
    stretch_ = {};
    alternative_.clear();
    // A word stretched with ー, as chat writes it, converts as the word with
    // the ー kept: on its end (いいよー), or before a final い or う (ばいばーい).
    // Not where Anthy reads it whole as a katakana word (カレー, コーヒー).
    constexpr std::string_view long_vowel = "ー";
    auto base = reading;
    Stretch stretch;
    while (std::string_view(base).ends_with(long_vowel) && g_utf8_strlen(base.c_str(), -1) > 2) {
        base.resize(base.size() - long_vowel.size());
        stretch.marks += long_vowel;
    }
    for (const std::string_view vowel : {"い", "う"}) {
        if (stretch.empty() && std::string_view(base).ends_with(vowel)) {
            auto head = base.substr(0, base.size() - vowel.size());
            std::string marks;
            while (std::string_view(head).ends_with(long_vowel) && g_utf8_strlen(head.c_str(), -1) > 2) {
                head.resize(head.size() - long_vowel.size());
                marks += long_vowel;
            }
            if (!marks.empty()) {
                base = head + std::string(vowel);
                stretch = {marks, std::string(vowel)};
            }
        }
    }
    if (!stretch.empty()) {
        auto whole = imported(converter().convert(reading));
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
    auto converted = romaji_ ? carried_n(base) : base;
    auto result = imported(converter().convert(converted));
    if (converted != base) {
        auto typed = imported(converter().convert(base));
        if (result.size() < typed.size()) {
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
void JapaneseComposer::stretch_segment(Segment& segment, const Stretch& stretch) {
    // Those that can't take it go: 倍々 for ばいばい, as ばいばーい.
    segment.reading = stretch.apply(segment.reading);
    std::vector<std::string> candidates;
    for (const auto& candidate : segment.candidates) {
        if (auto stretched = stretch.apply(candidate);
            !stretched.empty() &&
            std::find(candidates.begin(), candidates.end(), stretched) == candidates.end()) {
            candidates.push_back(std::move(stretched));
        }
    }
    segment.candidates = std::move(candidates);
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
    if (!resized.empty() && !stretch_.empty()) {
        stretch_segment(resized.back(), stretch_); // Anthy converted it without.
    }
    resized_ = true;
    if (resized.empty() || static_cast<int>(resized.size()) <= active_) {
        return;
    }
    // The segments before the active one keep their choices.
    for (int i = 0; i < active_; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (resized[index].reading == segments_[index].reading) {
            resized[index].selected = segments_[index].selected;
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
    // Only while the emoji is still all there is: typing after a refused
    // commit makes it part of the reading.
    return emoji_choice_ && reading_ == emoji_choice_->emoji && pending_.empty() && !converting()
               ? emoji_choice_->reading
               : reading_;
}
std::string JapaneseComposer::typed_reading() const {
    return predicted_ && !live_.reading.empty() ? live_.reading : reopen_reading();
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
bool JapaneseWords::completion_leads(const std::string& reading, int cost) const {
    // A word read so leads its completions unless they are clearly commoner:
    // おめでとう (1842) leads おめでと (4192), but 涙 keeps its place before
    // 涙する and だめ before ダメージ. A word of two kana always leads, as a
    // phone's does: はな is 花 before 話 (はなし).
    // A reading ending in った is a verb's past, as typed: あった before
    // あったかい, かった before かったるい, やった before ヤッター.
    if (std::string_view(reading).ends_with("った")) {
        return false;
    }
    const int own = own_cost(reading);
    if (own == std::numeric_limits<int>::max()) {
        return true;
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
    // the commoner word: おもしろ to 面白い.
    const bool stem = !found.empty() && reading_of(*found.front()) == reading + "い" &&
                      exact != std::numeric_limits<int>::max() &&
                      g_utf8_strlen(reading.c_str(), -1) > 2 &&
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

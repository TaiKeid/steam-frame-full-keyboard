#include "framekeyboard/japanese.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <glib.h>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

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
            "- ー , 、 . 。 [ 「 ] 」 / ・");
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
class AnthyConverter final : public JapaneseConverter {
  public:
    AnthyConverter() {
        auto& api = library();
        context_ = api.create();
        if (!context_) {
            throw std::runtime_error("Cannot create Japanese conversion context");
        }
        api.encoding(context_, 2); // ANTHY_UTF8_ENCODING
    }
    ~AnthyConverter() override { library().release(context_); }
    std::vector<Segment> convert(const std::string& reading) override {
        if (library().set(context_, reading.c_str()) != 0) {
            throw std::runtime_error("Japanese conversion failed");
        }
        return segments();
    }
    std::vector<Segment> resize(int index, int delta) override {
        auto& api = library();
        Stat stat{};
        if (api.stat(context_, &stat) != 0 || index < 0 || index >= stat.segments) {
            return {};
        }
        // Anthy leaves the segments as they were when the change is impossible.
        api.resize(context_, index, delta);
        return segments();
    }

  private:
    struct Stat {
        int segments;
    };
    struct SegmentStat {
        int candidates, length;
    };
    struct Library {
        void* handle{};
        int (*init)(){};
        void (*quit)(){};
        void* (*create)(){};
        void (*release)(void*){};
        int (*encoding)(void*, int){};
        int (*set)(void*, const char*){};
        int (*stat)(void*, Stat*){};
        int (*segment_stat)(void*, int, SegmentStat*){};
        int (*get)(void*, int, int, char*, int){};
        void (*resize)(void*, int, int){};
        template <class T> void symbol(T& fn, const char* name) {
            fn = reinterpret_cast<T>(dlsym(handle, name));
            if (!fn) {
                throw std::runtime_error("Installed Anthy has an incompatible API");
            }
        }
        Library() {
            handle = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                throw std::runtime_error("Japanese conversion needs the Anthy library");
            }
            try {
                symbol(init, "anthy_init");
                symbol(quit, "anthy_quit");
                symbol(create, "anthy_create_context");
                symbol(release, "anthy_release_context");
                symbol(encoding, "anthy_context_set_encoding");
                symbol(set, "anthy_set_string");
                symbol(stat, "anthy_get_stat");
                symbol(segment_stat, "anthy_get_segment_stat");
                symbol(get, "anthy_get_segment");
                symbol(resize, "anthy_resize_segment");
                if (init() != 0) {
                    throw std::runtime_error("Cannot initialize the Anthy dictionary");
                }
            } catch (...) {
                dlclose(handle);
                throw;
            }
        }
        ~Library() {
            quit();
            dlclose(handle);
        }
    };
    // Anthy initialization is process-wide. Share its lifetime across previews/tests.
    static Library& library() {
        static Library value;
        return value;
    }
    // Anthy's segment text at `candidate`, or its reading at -1
    // (NTH_UNCONVERTED_CANDIDATE).
    std::string text(int segment, int candidate) {
        char buffer[1024]{};
        if (library().get(context_, segment, candidate, buffer, sizeof(buffer)) < 0 ||
            !g_utf8_validate(buffer, -1, nullptr)) {
            throw std::runtime_error("Invalid Japanese conversion candidate");
        }
        return buffer;
    }
    std::vector<Segment> segments() {
        auto& api = library();
        Stat stat{};
        if (api.stat(context_, &stat) != 0 || stat.segments <= 0 || stat.segments > 32) {
            throw std::runtime_error("Japanese conversion failed");
        }
        std::vector<Segment> result;
        for (int s = 0; s < stat.segments; ++s) {
            SegmentStat info{};
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
} // namespace
JapaneseComposer::JapaneseComposer(Factory factory) : factory_(std::move(factory)) {
}
JapaneseComposer::~JapaneseComposer() = default;
JapaneseConverter& JapaneseComposer::converter() {
    if (!converter_) {
        converter_ = factory_ ? factory_() : std::make_unique<AnthyConverter>();
    }
    return *converter_;
}
std::vector<JapaneseComposer::Segment>
JapaneseComposer::imported(std::vector<JapaneseConverter::Segment> segments) const {
    std::vector<Segment> result;
    for (auto& from : segments) {
        if (from.candidates.empty()) {
            throw std::runtime_error("No conversion candidates");
        }
        Segment segment{std::move(from.reading), std::move(from.candidates)};
        // The segment's reading in both scripts, after Anthy's own candidates.
        for (const auto& script :
             {in_script(segment.reading, false), in_script(segment.reading, true)}) {
            if (!script.empty() && segment.candidates.size() < 130 &&
                std::find(segment.candidates.begin(), segment.candidates.end(), script) ==
                    segment.candidates.end()) {
                segment.candidates.push_back(script);
            }
        }
        result.push_back(std::move(segment));
    }
    return result;
}
std::vector<JapaneseComposer::Segment> JapaneseComposer::fresh(const std::string& reading) {
    converted_.clear();
    auto result = imported(converter().convert(reading));
    converted_ = reading;
    resized_ = false;
    return result;
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
    return cursor_ + (pending_ == "nn" ? std::string("ん").size() : pending_.size());
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
                // nn commits one ん; retain the second n for nna/nni etc.
                if (pending_.size() == 2) {
                    if (finish) {
                        append("ん");
                        pending_.clear();
                    }
                    return;
                }
                append("ん");
                pending_.erase(0, std::string("aiueoy").find(pending_[2]) != std::string::npos ? 1 : 2);
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
    if (!pending_.empty() && pending_ != "n" && pending_ != "nn") {
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
    add(whole, Live::Source::Whole);
    const auto& first = live.segments.front().candidates;
    for (std::size_t i = 0; i < first.size(); ++i) {
        add(first[i], Live::Source::First, static_cast<int>(i));
    }
    add(in_script(reading, false), Live::Source::Hiragana);
    add(in_script(reading, true), Live::Source::Katakana);
    live_ = std::move(live);
}
bool JapaneseComposer::choose_live(int index) {
    if (!live_current() || index < 0 || index >= static_cast<int>(live_.texts.size())) {
        return false;
    }
    drain(true); // A pending n is the ん the list was converted with.
    const auto [source, at] = live_.sources[static_cast<std::size_t>(index)];
    if (source == Live::Source::Hiragana || source == Live::Source::Katakana) {
        reading_ = in_script(reading_, source == Live::Source::Katakana);
        cursor_ = reading_.size();
        return true;
    }
    convert();
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
}
void JapaneseComposer::cancel() {
    unconvert();
    reading_.clear();
    pending_.clear();
    cursor_ = 0;
    live_ = {};
}
std::string JapaneseComposer::preedit(bool mark_active) const {
    if (!converting()) {
        // Romaji still waiting shows where it was typed, at the cursor.
        return reading_.substr(0, cursor_) + (pending_ == "nn" ? "ん" : pending_) +
               reading_.substr(cursor_);
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
std::vector<std::string> JapaneseComposer::candidates() const {
    return converting() ? segments_[static_cast<std::size_t>(active_)].candidates
                        : std::vector<std::string>{};
}
int JapaneseComposer::selected() const {
    return converting() ? segments_[static_cast<std::size_t>(active_)].selected : 0;
}
} // namespace framekeyboard

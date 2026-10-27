#include "framekeyboard/cheonjiin.hpp"

#include <algorithm>
#include <glib.h>
#include <map>
#include <stdexcept>
#include <string_view>

namespace framekeyboard {
namespace {
// The reading's bound, in units, as for the other composers.
constexpr std::size_t max_units = 30;
// Strokes are kept as i (ㅣ, the person), . (ㆍ, heaven) and - (ㅡ, earth).
char stroke(const std::string& text) {
    return text == "ㅣ" ? 'i' : text == "ㆍ" ? '.' : text == "ㅡ" ? '-' : 0;
}
// Every stroke sequence that is a vowel or on the way to one. The lone ㆍ and
// ㆍㆍ are not vowels yet and map to 0.
const std::map<std::string, char32_t>& vowels() {
    static const std::map<std::string, char32_t> table{
        {"i", U'ㅣ'},    {".", 0},         {"..", 0},       {"-", U'ㅡ'},   {"i.", U'ㅏ'},
        {".i", U'ㅓ'},   {".-", U'ㅗ'},    {"-.", U'ㅜ'},   {"i..", U'ㅑ'}, {"..i", U'ㅕ'},
        {"..-", U'ㅛ'},  {"-..", U'ㅠ'},   {"i.i", U'ㅐ'},  {".ii", U'ㅔ'}, {"i..i", U'ㅒ'},
        {"..ii", U'ㅖ'}, {"-i", U'ㅢ'},    {".-i", U'ㅚ'},  {"-.i", U'ㅟ'}, {".-i.", U'ㅘ'},
        {"-..i", U'ㅝ'}, {".-i.i", U'ㅙ'}, {"-..ii", U'ㅞ'}};
    return table;
}
// Two vowels that two-set Hangul joins into one. A flicked vowel extends the
// previous one only for these, never just because its strokes would.
constexpr std::pair<char32_t, char32_t> compound_vowels[] = {
    {U'ㅗ', U'ㅏ'}, {U'ㅗ', U'ㅐ'}, {U'ㅗ', U'ㅣ'}, {U'ㅜ', U'ㅓ'},
    {U'ㅜ', U'ㅔ'}, {U'ㅜ', U'ㅣ'}, {U'ㅡ', U'ㅣ'}};
constexpr std::u32string_view initials = U"ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ";
// Index 0 is no final; ㄸ, ㅃ and ㅉ cannot end a syllable.
constexpr std::u32string_view finals = U" ㄱㄲㄳㄴㄵㄶㄷㄹㄺㄻㄼㄽㄾㄿㅀㅁㅂㅄㅅㅆㅇㅈㅊㅋㅌㅍㅎ";
char32_t cluster(char32_t first, char32_t second) {
    static const std::map<std::pair<char32_t, char32_t>, char32_t> table{
        {{U'ㄱ', U'ㅅ'}, U'ㄳ'}, {{U'ㄴ', U'ㅈ'}, U'ㄵ'}, {{U'ㄴ', U'ㅎ'}, U'ㄶ'},
        {{U'ㄹ', U'ㄱ'}, U'ㄺ'}, {{U'ㄹ', U'ㅁ'}, U'ㄻ'}, {{U'ㄹ', U'ㅂ'}, U'ㄼ'},
        {{U'ㄹ', U'ㅅ'}, U'ㄽ'}, {{U'ㄹ', U'ㅌ'}, U'ㄾ'}, {{U'ㄹ', U'ㅍ'}, U'ㄿ'},
        {{U'ㄹ', U'ㅎ'}, U'ㅀ'}, {{U'ㅂ', U'ㅅ'}, U'ㅄ'}};
    const auto it = table.find({first, second});
    return it == table.end() ? 0 : it->second;
}
char32_t decode(const std::string& text) {
    return g_utf8_strlen(text.c_str(), -1) == 1 ? g_utf8_get_char(text.c_str()) : 0;
}
std::string encode(char32_t c) {
    char buffer[6];
    return {buffer, static_cast<std::size_t>(g_unichar_to_utf8(c, buffer))};
}
// The strokes of a whole vowel, for a flick that types one.
std::string strokes_of(char32_t vowel) {
    for (const auto& [strokes, jamo] : vowels()) {
        if (jamo == vowel && vowel) {
            return strokes;
        }
    }
    return {};
}
char32_t vowel(const std::string& strokes) {
    const auto it = vowels().find(strokes);
    return it == vowels().end() ? 0 : it->second;
}
// A vowel as shown: its jamo, or the unfinished ㆍ strokes themselves.
std::string vowel_text(const std::string& strokes) {
    const auto jamo = vowel(strokes);
    return jamo ? encode(jamo) : strokes == "." ? "ㆍ" : "ㆍㆍ";
}
// One syllable being assembled: initial, vowel and up to two final jamo.
struct Syllable {
    char32_t initial{}, medial{}, final{}, cluster_end{};
    std::string text() const {
        if (initial && medial) {
            const auto last = cluster_end ? cluster(final, cluster_end) : final;
            const auto l = initials.find(initial);
            const auto v = medial - U'ㅏ'; // ㅏ … ㅣ are consecutive.
            const auto t = last ? finals.find(last) : 0;
            return encode(static_cast<char32_t>(0xAC00 + (l * 21 + v) * 28 + t));
        }
        return initial ? encode(initial) : medial ? encode(medial) : std::string{};
    }
};
} // namespace
std::string CheonjiinComposer::Unit::strokes() const {
    std::string result;
    for (const auto& step : steps) {
        result += step;
    }
    return result;
}
void CheonjiinComposer::add(const std::string& text) {
    if (text.empty()) {
        return;
    }
    Unit* last = units_.empty() ? nullptr : &units_.back();
    const bool after_vowel = last && last->kind == Unit::Kind::Vowel;
    if (const char s = stroke(text)) {
        // A stroke extends the previous vowel while the result still reads as one.
        if (after_vowel && vowels().contains(last->strokes() + s)) {
            last->steps.emplace_back(1, s);
            return;
        }
        if (units_.size() >= max_units) {
            throw std::runtime_error("Commit this composition before typing more");
        }
        units_.push_back({Unit::Kind::Vowel, {}, {std::string(1, s)}});
        return;
    }
    const auto jamo = decode(text);
    if (const auto strokes = strokes_of(jamo); !strokes.empty()) {
        const auto previous = after_vowel ? vowel(last->strokes()) : 0;
        if (previous && std::find(std::begin(compound_vowels), std::end(compound_vowels),
                                  std::pair{previous, jamo}) != std::end(compound_vowels)) {
            last->steps.push_back(strokes);
            return;
        }
        if (units_.size() >= max_units) {
            throw std::runtime_error("Commit this composition before typing more");
        }
        units_.push_back({Unit::Kind::Vowel, {}, {strokes}});
        return;
    }
    if (units_.size() >= max_units) {
        throw std::runtime_error("Commit this composition before typing more");
    }
    const bool consonant = jamo && initials.find(jamo) != std::u32string_view::npos;
    units_.push_back({consonant ? Unit::Kind::Consonant : Unit::Kind::Other, text, {}});
}
void CheonjiinComposer::replace(const std::string& text) {
    if (!units_.empty()) {
        units_.pop_back();
    }
    add(text);
}
void CheonjiinComposer::backspace() {
    if (units_.empty()) {
        return;
    }
    auto& last = units_.back();
    if (last.kind == Unit::Kind::Vowel && last.steps.size() > 1) {
        last.steps.pop_back();
    } else {
        units_.pop_back();
    }
}
void CheonjiinComposer::cancel() {
    units_.clear();
}
bool CheonjiinComposer::empty() const {
    return units_.empty();
}
std::string CheonjiinComposer::last() const {
    if (units_.empty()) {
        return {};
    }
    const auto& unit = units_.back();
    return unit.kind == Unit::Kind::Vowel ? vowel_text(unit.strokes()) : unit.text;
}
std::string CheonjiinComposer::text() const {
    std::string result;
    Syllable s;
    auto flush = [&] {
        result += s.text();
        s = {};
    };
    for (const auto& unit : units_) {
        if (unit.kind == Unit::Kind::Consonant) {
            const auto c = decode(unit.text);
            if (s.initial && s.medial && !s.final && finals.find(c) != std::u32string_view::npos) {
                s.final = c;
            } else if (s.initial && s.medial && s.final && !s.cluster_end && cluster(s.final, c)) {
                s.cluster_end = c;
            } else {
                flush();
                s.initial = c;
            }
            continue;
        }
        const auto v = unit.kind == Unit::Kind::Vowel ? vowel(unit.strokes()) : 0;
        if (!v) {
            // Pending strokes and punctuation end the syllable and show as typed.
            flush();
            result += unit.kind == Unit::Kind::Vowel ? vowel_text(unit.strokes()) : unit.text;
            continue;
        }
        if (s.initial && !s.medial) {
            s.medial = v;
        } else if (s.final) {
            // A vowel takes the previous syllable's last final as its initial.
            char32_t moved = s.cluster_end ? s.cluster_end : s.final;
            (s.cluster_end ? s.cluster_end : s.final) = 0;
            flush();
            s.initial = moved;
            s.medial = v;
        } else {
            flush();
            s.medial = v;
        }
    }
    flush();
    return result;
}
} // namespace framekeyboard

#include "framekeyboard/word_suggestions.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <glib.h>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace framekeyboard {
namespace {
namespace fs = std::filesystem;
bool valid(std::string_view text) {
    return g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}
std::size_t characters(std::string_view text) {
    return static_cast<std::size_t>(g_utf8_strlen(text.data(), static_cast<gssize>(text.size())));
}
// A word as kept: valid UTF-8, two characters or more, without the file's separators.
bool keepable(std::string_view word) {
    return valid(word) && word.find_first_of("\t\n\r") == std::string_view::npos &&
           characters(word) >= 2 && characters(word) <= LearnedWords::longest;
}
bool write_all(int fd, const std::string& data) {
    for (std::size_t offset = 0; offset < data.size();) {
        const auto written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}
fs::path temporary_of(const fs::path& file) {
    auto temporary = file;
    temporary += ".tmp";
    return temporary;
}
// The user's own uses raise a listed word, by less for each further use.
int bonus(std::uint32_t uses) {
    return uses == 0 ? 0 : std::min(200, static_cast<int>(40 * std::log2(1.0 + uses)));
}
// A word the list lacks, once learned, ranks as a middling listed word.
constexpr int learned_frequency = 60;
} // namespace

std::string word_key(std::string_view word) {
    if (!valid(word)) {
        return {};
    }
    auto* lower = g_utf8_strdown(word.data(), static_cast<gssize>(word.size()));
    std::string key(lower);
    g_free(lower);
    return key;
}
bool word_character(std::uint32_t character) {
    return g_unichar_isalpha(character) || g_unichar_ismark(character) || character == '\'' ||
           character == 0x2019; // ’
}

namespace {
void apply(const WordList::Respelling& respell, std::string& spelling) {
    for (const auto& [from, to] : respell) {
        for (auto at = spelling.find(from); !from.empty() && at != std::string::npos;
             at = spelling.find(from, at + to.size())) {
            spelling.replace(at, from.size(), to);
        }
    }
}
} // namespace
WordList WordList::parse(std::istream& input, const Respelling& respell, std::istream* offensive) {
    struct Parsed {
        std::string key, spelling;
        int frequency{};
    };
    std::set<std::string, std::less<>> held_back;
    std::string line;
    while (offensive && std::getline(*offensive, line)) {
        if (!line.empty() && line.front() != '#') {
            apply(respell, line);
            held_back.insert(word_key(line));
        }
    }
    std::vector<Parsed> parsed;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == 0 || tab == std::string::npos) {
            continue;
        }
        std::string spelling = line.substr(0, tab);
        apply(respell, spelling);
        int frequency = 0;
        try {
            frequency = std::clamp(std::stoi(line.substr(tab + 1)), 0, 255);
        } catch (const std::exception&) {
            continue;
        }
        auto key = word_key(spelling);
        if (key.empty() || key.size() > 0xffff || spelling.size() > 0xffff) {
            continue;
        }
        parsed.push_back({std::move(key), spelling, frequency});
    }
    // Each key once, with its commonest spelling: he before He.
    std::sort(parsed.begin(), parsed.end(), [](const Parsed& a, const Parsed& b) {
        return a.key != b.key ? a.key < b.key : a.frequency > b.frequency;
    });
    WordList list;
    for (const auto& word : parsed) {
        if (!list.entries_.empty() && list.word(list.entries_.back()).key == word.key) {
            continue;
        }
        Entry entry;
        entry.key = static_cast<std::uint32_t>(list.text_.size());
        entry.key_length = static_cast<std::uint16_t>(word.key.size());
        list.text_ += word.key;
        entry.spelling = entry.key;
        entry.spelling_length = entry.key_length;
        if (word.spelling != word.key) {
            entry.spelling = static_cast<std::uint32_t>(list.text_.size());
            entry.spelling_length = static_cast<std::uint16_t>(word.spelling.size());
            list.text_ += word.spelling;
        }
        entry.frequency = static_cast<std::uint8_t>(word.frequency);
        entry.offensive = held_back.contains(word.key);
        list.entries_.push_back(entry);
    }
    return list;
}
WordList WordList::load(const fs::path& path, const Respelling& respell, const fs::path& offensive) {
    std::ifstream input(path);
    std::ifstream held_back;
    if (!offensive.empty()) {
        held_back.open(offensive);
    }
    return input ? parse(input, respell, held_back.is_open() ? &held_back : nullptr) : WordList{};
}
WordList::Word WordList::word(const Entry& entry) const {
    return {std::string_view(text_).substr(entry.key, entry.key_length),
            std::string_view(text_).substr(entry.spelling, entry.spelling_length), entry.frequency,
            entry.offensive};
}
std::vector<WordList::Word> WordList::starting(std::string_view prefix, std::size_t most) const {
    const auto first = std::lower_bound(
        entries_.begin(), entries_.end(), prefix,
        [&](const Entry& entry, std::string_view key) { return word(entry).key < key; });
    std::vector<Word> found;
    for (auto at = first; at != entries_.end() && word(*at).key.starts_with(prefix); ++at) {
        found.push_back(word(*at));
    }
    const auto keep = std::min(most, found.size());
    std::partial_sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(keep), found.end(),
                      [](const Word& a, const Word& b) {
                          if (a.offensive != b.offensive) {
                              return b.offensive;
                          }
                          return a.frequency != b.frequency ? a.frequency > b.frequency : a.key < b.key;
                      });
    found.resize(keep);
    return found;
}
std::optional<WordList::Word> WordList::find(std::string_view key) const {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), key,
        [&](const Entry& entry, std::string_view wanted) { return word(entry).key < wanted; });
    if (at == entries_.end() || word(*at).key != key) {
        return std::nullopt;
    }
    return word(*at);
}

void LearnedWords::load() const {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    if (file_.empty()) {
        return;
    }
    // spelling<TAB>uses<TAB>stamp; a damaged line is skipped.
    std::ifstream input(file_);
    std::string line;
    while (std::getline(input, line) && words_.size() < most_words) {
        std::istringstream fields(line);
        Use use;
        std::string uses, stamp;
        if (!std::getline(fields, use.spelling, '\t') || !std::getline(fields, uses, '\t') ||
            !std::getline(fields, stamp) || !keepable(use.spelling)) {
            continue;
        }
        try {
            use.uses = static_cast<std::uint32_t>(std::min(std::stoul(uses), 1000000ul));
            use.stamp = std::stoull(stamp);
        } catch (const std::exception&) {
            continue;
        }
        use.key = word_key(use.spelling);
        clock_ = std::max(clock_, use.stamp);
        words_.push_back(std::move(use));
    }
}
void LearnedWords::record(std::string_view word) {
    if (file_.empty() || !keepable(word)) {
        return;
    }
    load();
    const auto key = word_key(word);
    const auto found =
        std::find_if(words_.begin(), words_.end(), [&](const Use& use) { return use.key == key; });
    if (found != words_.end()) {
        found->uses = std::min<std::uint32_t>(found->uses + 1, 1000000);
        found->stamp = ++clock_;
        found->spelling = std::string(word); // As last typed: iPhone stays iPhone.
    } else {
        words_.push_back({key, std::string(word), 1, ++clock_});
        if (words_.size() > most_words) {
            // The one used longest ago goes.
            words_.erase(std::min_element(words_.begin(), words_.end(),
                                          [](const Use& a, const Use& b) { return a.stamp < b.stamp; }));
        }
    }
    dirty_ = true;
}
std::uint32_t LearnedWords::uses(std::string_view key) const {
    load();
    const auto found =
        std::find_if(words_.begin(), words_.end(), [&](const Use& use) { return use.key == key; });
    return found == words_.end() ? 0 : found->uses;
}
std::vector<const LearnedWords::Use*> LearnedWords::starting(std::string_view prefix) const {
    load();
    std::vector<const Use*> found;
    for (const auto& use : words_) {
        if (std::string_view(use.key).starts_with(prefix)) {
            found.push_back(&use);
        }
    }
    return found;
}
bool LearnedWords::empty() const {
    load();
    return words_.empty();
}
std::size_t LearnedWords::size() const {
    load();
    return words_.size();
}
void LearnedWords::clear() {
    words_.clear();
    loaded_ = true;
    clock_ = 0;
    dirty_ = false;
    if (!file_.empty()) {
        std::error_code ignored;
        fs::remove(file_, ignored);
        fs::remove(temporary_of(file_), ignored);
    }
}
bool LearnedWords::save() {
    if (!dirty_ || file_.empty()) {
        return true;
    }
    // Written whole beside it, then put in its place. The words are the
    // user's own: their folder and file are the owner's alone from their
    // creation, and a failed write leaves nothing behind.
    std::error_code error;
    const auto folder = file_.parent_path();
    if (!folder.empty() && !fs::exists(folder, error)) {
        if (mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST) {
            return false;
        }
    }
    std::ostringstream text;
    for (const auto& use : words_) {
        text << use.spelling << '\t' << use.uses << '\t' << use.stamp << '\n';
    }
    const auto temporary = temporary_of(file_);
    unlink(temporary.c_str());
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    // No fsync: this runs on the VR thread, and a sync could stall a frame.
    const bool written = write_all(fd, text.str());
    if (close(fd) != 0 || !written) {
        unlink(temporary.c_str());
        return false;
    }
    fs::rename(temporary, file_, error);
    if (error) {
        unlink(temporary.c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

std::vector<std::string> complete_word(const WordList& list, const LearnedWords& learned,
                                       std::string_view typed, std::size_t most) {
    const auto key = word_key(typed);
    if (key.empty() || most == 0) {
        return {};
    }
    // Candidates by key: their spelling and score.
    std::map<std::string, std::pair<std::string, int>, std::less<>> scored;
    // An offensive word the user hasn't used ranks below every other one;
    // once used, it ranks as any word, so whoever swears trains it back.
    const auto score = [&](const WordList::Word& word, std::uint32_t uses) {
        return word.offensive && uses == 0 ? word.frequency - 256 : word.frequency + bonus(uses);
    };
    for (const auto& word : list.starting(key, 32)) {
        if (word.key != key) {
            scored[std::string(word.key)] = {std::string(word.spelling),
                                             score(word, learned.uses(word.key))};
        }
    }
    // The user's words: listed ones the top above missed, and new ones used often enough.
    for (const auto* use : learned.starting(key)) {
        if (use->key == key || scored.contains(use->key)) {
            continue;
        }
        if (const auto listed = list.find(use->key)) {
            scored[use->key] = {std::string(listed->spelling), score(*listed, use->uses)};
        } else if (use->uses >= LearnedWords::new_word_uses) {
            scored[use->key] = {use->spelling, learned_frequency + bonus(use->uses)};
        }
    }
    std::vector<std::pair<std::string, int>> ranked;
    for (auto& [ignored, entry] : scored) {
        ranked.push_back(std::move(entry));
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first.size() < b.first.size();
    });
    // What was typed stays as typed; only the rest of the word is added.
    const auto typed_length = characters(typed);
    bool capitals = typed_length >= 2;
    for (const char* p = typed.data(); capitals && p < typed.data() + typed.size();
         p = g_utf8_next_char(p)) {
        capitals = !g_unichar_islower(g_utf8_get_char(p));
    }
    std::vector<std::string> result;
    for (const auto& [spelling, score] : ranked) {
        if (result.size() == most) {
            break;
        }
        if (characters(spelling) <= typed_length) {
            continue;
        }
        const auto cut = static_cast<std::size_t>(
            g_utf8_offset_to_pointer(spelling.c_str(), static_cast<glong>(typed_length)) -
            spelling.c_str());
        if (word_key(std::string_view(spelling).substr(0, cut)) != key) {
            continue; // Lower-casing changed its length: no safe split.
        }
        auto rest = spelling.substr(cut);
        if (capitals) {
            auto* upper = g_utf8_strup(rest.c_str(), -1);
            rest = upper;
            g_free(upper);
        }
        result.push_back(std::string(typed) + rest);
    }
    return result;
}
} // namespace framekeyboard

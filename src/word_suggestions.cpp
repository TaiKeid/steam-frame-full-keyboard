#include "framekeyboard/word_suggestions.hpp"
#include "framekeyboard/private_file.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <glib.h>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

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
    std::set<std::string, std::less<>> held_back;
    std::string line;
    while (offensive && std::getline(*offensive, line)) {
        if (!line.empty() && line.front() != '#') {
            apply(respell, line);
            held_back.insert(word_key(line));
        }
    }
    WordList list;
    list.respell_ = respell;
    // Every key and spelling goes straight into the list's one buffer: a
    // string for each word would take several times the list's memory.
    std::string spelling;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == 0 || tab == std::string::npos) {
            continue;
        }
        int frequency = 0;
        const char* const number = line.data() + tab + 1;
        const auto [end, error] = std::from_chars(number, line.data() + line.size(), frequency);
        if (error != std::errc{} || end == number) {
            continue;
        }
        spelling.assign(line, 0, tab);
        apply(respell, spelling);
        const auto key = word_key(spelling);
        if (key.empty() || key.size() > 0xffff || spelling.size() > 0xffff ||
            list.text_.size() + key.size() + spelling.size() > UINT32_MAX) {
            continue;
        }
        Entry entry;
        entry.key = static_cast<std::uint32_t>(list.text_.size());
        entry.key_length = static_cast<std::uint16_t>(key.size());
        list.text_ += key;
        entry.spelling = entry.key;
        entry.spelling_length = entry.key_length;
        if (spelling != key) {
            entry.spelling = static_cast<std::uint32_t>(list.text_.size());
            entry.spelling_length = static_cast<std::uint16_t>(spelling.size());
            list.text_ += spelling;
        }
        entry.frequency = static_cast<std::uint8_t>(std::clamp(frequency, 0, 255));
        entry.offensive = held_back.contains(key);
        list.entries_.push_back(entry);
    }
    // Each key once, with its commonest spelling: he before He.
    auto& entries = list.entries_;
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        const auto a_key = list.word(a).key, b_key = list.word(b).key;
        return a_key != b_key ? a_key < b_key : a.frequency > b.frequency;
    });
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [&](const Entry& a, const Entry& b) {
                                  return list.word(a).key == list.word(b).key;
                              }),
                  entries.end());
    entries.shrink_to_fit();
    list.text_.shrink_to_fit();
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
    // Offensive words last, then commonest first.
    const auto better = [](const Word& a, const Word& b) {
        if (a.offensive != b.offensive) {
            return b.offensive;
        }
        return a.frequency != b.frequency ? a.frequency > b.frequency : a.key < b.key;
    };
    std::vector<Word> best; // A heap whose front is the worst kept.
    if (most == 0) {
        return best;
    }
    best.reserve(std::min<std::size_t>(most, 256) + 1);
    const auto first = std::lower_bound(
        entries_.begin(), entries_.end(), prefix,
        [&](const Entry& entry, std::string_view key) { return word(entry).key < key; });
    for (auto at = first; at != entries_.end(); ++at) {
        const auto found = word(*at);
        if (!found.key.starts_with(prefix)) {
            break;
        }
        // One letter can begin ten thousand words: only the best stay.
        if (best.size() < most) {
            best.push_back(found);
            std::push_heap(best.begin(), best.end(), better);
        } else if (better(found, best.front())) {
            std::pop_heap(best.begin(), best.end(), better);
            best.back() = found;
            std::push_heap(best.begin(), best.end(), better);
        }
    }
    std::sort_heap(best.begin(), best.end(), better);
    return best;
}
std::string WordList::respelled(std::string_view word) const {
    std::string spelling(word);
    apply(respell_, spelling);
    return spelling;
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
        words_.try_emplace(use.key, std::move(use));
    }
}
void LearnedWords::record(std::string_view word) {
    if (file_.empty() || !keepable(word)) {
        return;
    }
    load();
    auto key = word_key(word);
    if (const auto found = words_.find(key); found != words_.end()) {
        auto& use = found->second;
        use.uses = std::min<std::uint32_t>(use.uses + 1, 1000000);
        use.stamp = ++clock_;
        use.spelling = std::string(word); // As last typed: iPhone stays iPhone.
    } else {
        words_.try_emplace(key, Use{key, std::string(word), 1, ++clock_});
        if (words_.size() > most_words) {
            // The one used longest ago goes.
            words_.erase(
                std::min_element(words_.begin(), words_.end(), [](const auto& a, const auto& b) {
                    return a.second.stamp < b.second.stamp;
                }));
        }
    }
    dirty_ = true;
}
std::uint32_t LearnedWords::uses(std::string_view key) const {
    load();
    const auto found = words_.find(key);
    return found == words_.end() ? 0 : found->second.uses;
}
std::vector<const LearnedWords::Use*> LearnedWords::starting(std::string_view prefix) const {
    load();
    std::vector<const Use*> found;
    for (auto at = words_.lower_bound(prefix); at != words_.end() && at->first.starts_with(prefix);
         ++at) {
        found.push_back(&at->second);
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
        remove_private_file(file_);
    }
}
bool LearnedWords::save() {
    if (!dirty_ || file_.empty()) {
        return true;
    }
    // The words are the user's own: their folder and file are the owner's
    // alone from their creation.
    std::ostringstream text;
    for (const auto& [key, use] : words_) {
        text << use.spelling << '\t' << use.uses << '\t' << use.stamp << '\n';
    }
    if (!write_private_file(file_, text.str(), true)) {
        return false;
    }
    dirty_ = false;
    return true;
}

WordListLoader::~WordListLoader() {
    {
        std::lock_guard guard(lock_);
        stop_ = true;
        waiting_.reset();
    }
    wake_.notify_one();
    if (thread_.joinable()) {
        thread_.join(); // After a load under way, at most a quarter second.
    }
}
bool WordListLoader::request(Request request) {
    {
        std::lock_guard guard(lock_);
        waiting_ = std::move(request);
        done_.reset();
    }
    if (!thread_.joinable()) {
        try {
            thread_ = std::thread([this] { run(); });
        } catch (const std::system_error&) {
            std::lock_guard guard(lock_);
            waiting_.reset();
            return false;
        }
    }
    wake_.notify_one();
    return true;
}
std::optional<WordListLoader::Result> WordListLoader::take() {
    std::lock_guard guard(lock_);
    return std::exchange(done_, std::nullopt);
}
void WordListLoader::run() {
    for (;;) {
        Request request;
        {
            std::unique_lock guard(lock_);
            wake_.wait(guard, [&] { return stop_ || waiting_; });
            if (stop_) {
                return;
            }
            request = std::move(*std::exchange(waiting_, std::nullopt));
        }
        Result result{request.id,
                      std::make_shared<const WordList>(
                          WordList::load(request.list, request.respell, request.offensive)),
                      LearnedWords(request.learned)};
        result.learned.size(); // Read here, not on the VR thread.
#if defined(__GLIBC__)
        malloc_trim(0); // Parsing's spare memory goes back.
#endif
        std::lock_guard guard(lock_);
        if (!waiting_) { // A newer request makes this one moot.
            done_ = std::move(result);
        }
    }
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
    // The user's words: listed ones the top above missed, and new ones used
    // often enough, as the list writes them (Straße learned in German is
    // Strasse in Swiss German, which shares its words).
    // Only the most used of them can outrank the list's best: one used
    // rarely, and missed by the top above, ranks below those.
    auto mine = learned.starting(key);
    constexpr std::size_t considered = 64;
    if (mine.size() > considered) {
        std::partial_sort(mine.begin(), mine.begin() + considered, mine.end(),
                          [](const auto* a, const auto* b) { return a->uses > b->uses; });
        mine.resize(considered);
    }
    std::string respelled_spelling, respelled_key;
    for (const auto* use : mine) {
        std::string_view spelling = use->spelling, use_key = use->key;
        if (list.respells()) {
            respelled_spelling = list.respelled(use->spelling);
            respelled_key = word_key(respelled_spelling);
            spelling = respelled_spelling;
            use_key = respelled_key;
        }
        if (use_key == key || scored.contains(use_key)) {
            continue;
        }
        if (const auto listed = list.find(use_key)) {
            scored[std::string(use_key)] = {std::string(listed->spelling), score(*listed, use->uses)};
        } else if (use->uses >= LearnedWords::new_word_uses) {
            scored[std::string(use_key)] = {std::string(spelling), learned_frequency + bonus(use->uses)};
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
    // Capitals after two capital letters and no small one: HEL, not I' or C'.
    int upper = 0;
    bool lower = false;
    for (const char* p = typed.data(); p < typed.data() + typed.size(); p = g_utf8_next_char(p)) {
        const auto character = g_utf8_get_char(p);
        upper += g_unichar_isupper(character) ? 1 : 0;
        lower |= g_unichar_islower(character) != 0;
    }
    const bool capitals = upper >= 2 && !lower;
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

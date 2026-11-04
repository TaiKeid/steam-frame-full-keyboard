#include "framekeyboard/next_words.hpp"

#include <algorithm>
#include <fstream>
#include <glib.h>
#include <sstream>
#include <system_error>

namespace framekeyboard {
namespace {
namespace fs = std::filesystem;
// One phrase as kept: valid UTF-8, without the file's separators.
bool keepable(const std::string& text) {
    return !text.empty() && g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr) &&
           text.find_first_of("\t\n\r") == std::string::npos &&
           static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1)) <= NextWords::longest;
}
} // namespace
void NextWords::load() const {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    if (file_.empty()) {
        return;
    }
    // previous<TAB>next<TAB>count<TAB>stamp; a damaged line is skipped.
    std::ifstream input(file_);
    std::string line;
    while (std::getline(input, line) && pairs_.size() < most_pairs) {
        std::istringstream fields(line);
        Pair pair;
        std::string count, stamp;
        if (!std::getline(fields, pair.previous, '\t') || !std::getline(fields, pair.next, '\t') ||
            !std::getline(fields, count, '\t') || !std::getline(fields, stamp) ||
            !keepable(pair.previous) || !keepable(pair.next)) {
            continue;
        }
        try {
            pair.count = static_cast<std::uint32_t>(std::min(std::stoul(count), 1000000ul));
            pair.stamp = std::stoull(stamp);
        } catch (const std::exception&) {
            continue;
        }
        clock_ = std::max(clock_, pair.stamp);
        pairs_.push_back(std::move(pair));
    }
}
void NextWords::record(const std::string& previous, const std::string& next) {
    if (file_.empty() || !keepable(previous) || !keepable(next)) {
        return;
    }
    load();
    const auto found = std::find_if(pairs_.begin(), pairs_.end(), [&](const Pair& pair) {
        return pair.previous == previous && pair.next == next;
    });
    if (found != pairs_.end()) {
        found->count = std::min<std::uint32_t>(found->count + 1, 1000000);
        found->stamp = ++clock_;
    } else {
        pairs_.push_back({previous, next, 1, ++clock_});
        if (pairs_.size() > most_pairs) {
            // The one recorded longest ago goes.
            pairs_.erase(
                std::min_element(pairs_.begin(), pairs_.end(),
                                 [](const Pair& a, const Pair& b) { return a.stamp < b.stamp; }));
        }
    }
    dirty_ = true;
}
std::vector<std::string> NextWords::after(const std::string& previous, std::size_t most) const {
    load();
    std::vector<const Pair*> found;
    for (const auto& pair : pairs_) {
        if (pair.previous == previous) {
            found.push_back(&pair);
        }
    }
    std::sort(found.begin(), found.end(), [](const Pair* a, const Pair* b) {
        return a->count != b->count ? a->count > b->count : a->stamp > b->stamp;
    });
    std::vector<std::string> result;
    for (std::size_t i = 0; i < found.size() && i < most; ++i) {
        result.push_back(found[i]->next);
    }
    return result;
}
bool NextWords::empty() const {
    load();
    return pairs_.empty();
}
void NextWords::clear() {
    pairs_.clear();
    loaded_ = true;
    clock_ = 0;
    dirty_ = false;
    if (!file_.empty()) {
        std::error_code error;
        fs::remove(file_, error);
    }
}
bool NextWords::save() {
    if (!dirty_ || file_.empty()) {
        return true;
    }
    // Written whole beside it, then put in its place.
    auto temporary = file_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        for (const auto& pair : pairs_) {
            output << pair.previous << '\t' << pair.next << '\t' << pair.count << '\t' << pair.stamp
                   << '\n';
        }
        if (!output.flush()) {
            return false;
        }
    }
    std::error_code error;
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    error);
    fs::rename(temporary, file_, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    dirty_ = false;
    return true;
}
} // namespace framekeyboard

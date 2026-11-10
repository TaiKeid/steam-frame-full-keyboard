#include "table_engine.hpp"
#include "framekeyboard/cjk.hpp"
#include "framekeyboard/localization.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <fstream>
#include <glib.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <tuple>
#include <unordered_set>

namespace framekeyboard {
namespace {
// The radical each letter key stands for, shown while composing.
constexpr std::array<const char*, 26> radicals{"日", "月", "金", "木", "水", "火", "土", "竹", "戈",
                                               "十", "大", "中", "一", "弓", "人", "心", "手", "口",
                                               "尸", "廿", "山", "女", "田", "難", "卜", "重"};
// SQLite is loaded on first use, like Anthy: a keyboard that never composes
// these languages needs neither the library nor a link-time dependency.
struct Sqlite {
    using Open = int (*)(const char*, void**, int, const char*);
    using Prepare = int (*)(void*, const char*, int, void**, const char**);
    using BindText = int (*)(void*, int, const char*, int, void (*)(void*));
    using Step = int (*)(void*);
    using ColumnText = const unsigned char* (*)(void*, int);
    using ColumnInt = std::int64_t (*)(void*, int);
    using Finalize = int (*)(void*);
    using Close = int (*)(void*);
    Open open{};
    Prepare prepare{};
    BindText bind{};
    Step step{};
    ColumnText column{};
    ColumnInt column_int{};
    Finalize finalize{};
    Close close{};
    bool ready{};
    Sqlite() {
        void* library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            return;
        }
        // Stays loaded for the process: it holds no state of ours between uses.
        const auto symbol = [&](auto& function, const char* name) {
            function =
                reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            return function != nullptr;
        };
        ready = symbol(open, "sqlite3_open_v2") && symbol(prepare, "sqlite3_prepare_v2") &&
                symbol(bind, "sqlite3_bind_text") && symbol(step, "sqlite3_step") &&
                symbol(column, "sqlite3_column_text") && symbol(column_int, "sqlite3_column_int64") &&
                symbol(finalize, "sqlite3_finalize") &&
                symbol(close, "sqlite3_close");
    }
};
const Sqlite& sqlite() {
    static const Sqlite library;
    return library;
}
constexpr int sqlite_row = 100;
constexpr int sqlite_open_readonly = 1, sqlite_open_uri = 0x40;
std::filesystem::path& table_directory() {
    static std::filesystem::path directory = "/usr/share/ibus-table/tables";
    return directory;
}
// Guards loading tables, the loaded ones and the rank list's path.
std::mutex& tables_mutex() {
    static std::mutex mutex;
    return mutex;
}
// The rank list's path, and what it held when last read: character to place,
// commonest first. Empty, as when the file is missing or malformed, keeps the
// tables' own order.
std::filesystem::path& ranks_file() {
    static std::filesystem::path file;
    return file;
}
// Each character's place in the list, and how many of the first places are
// ranked by usage; the rest of the repertoire follows unranked.
struct Ranks {
    std::unordered_map<std::string, std::uint32_t> places;
    std::size_t ranked{};
    bool empty() const { return places.empty(); }
};
// One character a line, each once, the ranked ones ending at one empty line
// (tools/build_chinese_ranks.py); without it, all are ranked. Anything else
// rejects the whole list rather than ranking by part of it.
Ranks read_ranks(const std::filesystem::path& file) {
    Ranks ranks;
    bool boundary = false;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() && !boundary && !ranks.places.empty()) {
            boundary = true;
            ranks.ranked = ranks.places.size();
            continue;
        }
        if (!g_utf8_validate(line.data(), static_cast<gssize>(line.size()), nullptr) ||
            g_utf8_strlen(line.c_str(), -1) != 1 ||
            !ranks.places.emplace(line, static_cast<std::uint32_t>(ranks.places.size())).second) {
            return {};
        }
    }
    if (!boundary) {
        ranks.ranked = ranks.places.size();
    }
    return ranks;
}
// Where a character goes among those its code shares: ideographs ranked by
// usage, by rank; the rest of the list's repertoire (Big5-HKSCS), by place;
// ideographs outside it, by block; compatibility ideographs; and symbols.
// Only Ranked counts as common (Table::find()).
enum class CharacterClass : std::uint64_t { Ranked, Listed, Unified, ExtensionA, Later, Compatibility, Other };
CharacterClass character_class(std::string_view text, const Ranks& ranks) {
    const auto point = g_utf8_get_char(text.data());
    if ((point >= 0xF900 && point <= 0xFAFF) || (point >= 0x2F800 && point <= 0x2FA1F)) {
        return CharacterClass::Compatibility;
    }
    const bool unified = point >= 0x4E00 && point <= 0x9FFF, extension_a = point >= 0x3400 && point <= 0x4DBF,
               later = point >= 0x20000 && point <= 0x3FFFF;
    if (!unified && !extension_a && !later) {
        return CharacterClass::Other;
    }
    // A phrase ranks by its first character.
    const auto first = std::string(text.substr(0, static_cast<std::size_t>(
                                                       g_utf8_next_char(text.data()) - text.data())));
    if (const auto place = ranks.places.find(first); place != ranks.places.end()) {
        return place->second < ranks.ranked ? CharacterClass::Ranked : CharacterClass::Listed;
    }
    return unified ? CharacterClass::Unified : extension_a ? CharacterClass::ExtensionA : CharacterClass::Later;
}
} // namespace

void set_table_directory(const std::filesystem::path& directory) {
    table_directory() = directory;
}
void set_character_ranks(const std::filesystem::path& file) {
    std::lock_guard lock(tables_mutex());
    ranks_file() = file;
}
std::optional<InputTable> table_for(const std::string& method) {
    const auto& directory = table_directory();
    if (method == "chinese-cangjie") {
        return InputTable{directory / "cangjie5.db", 5};
    }
    if (method == "chinese-quick") {
        return InputTable{directory / "quick5.db", 2};
    }
    return {};
}

struct TableEngine::Database {
    void* handle{};
    Database(const std::filesystem::path& table) {
        const auto& api = sqlite();
        if (!api.ready) {
            throw UiError(ui("ime.table-needs-sqlite"));
        }
        // Immutable: a system file that is never written, so no lock or journal.
        const auto uri = "file:" + table.string() + "?immutable=1";
        if (api.open(uri.c_str(), &handle, sqlite_open_readonly | sqlite_open_uri, nullptr) != 0) {
            release();
            throw UiError(ui("ime.table-unreadable"));
        }
    }
    void release() {
        if (handle) {
            sqlite().close(handle);
            handle = nullptr;
        }
    }
    ~Database() { release(); }
    // Calls `row` with each row's code, text, frequency and id. The tables
    // have no index on codes, so they are read whole, once (TableEngine::load()).
    template <class Row> bool each(Row&& row) {
        const auto& api = sqlite();
        void* query{};
        if (api.prepare(handle, "SELECT tabkeys, phrase, freq, id FROM phrases", -1, &query, nullptr) != 0) {
            return false;
        }
        while (api.step(query) == sqlite_row) {
            const auto* code = api.column(query, 0);
            const auto* text = api.column(query, 1);
            if (code && text) {
                row(reinterpret_cast<const char*>(code), reinterpret_cast<const char*>(text),
                    api.column_int(query, 2), api.column_int(query, 3));
            }
        }
        api.finalize(query);
        return true;
    }
    bool has_rows() {
        const auto& api = sqlite();
        void* query{};
        if (api.prepare(handle, "SELECT 1 FROM phrases LIMIT 1", -1, &query, nullptr) != 0) {
            return false;
        }
        const bool found = api.step(query) == sqlite_row;
        api.finalize(query);
        return found;
    }
};

// Each row's code and text, sorted by code, then in the order its candidates
// are offered. That order is one number per row, highest bits first: the
// table's own frequency tier, which marks a character's main codes (冇 is xb,
// and only a second choice at kb); then the character's class and rank; then
// its id. Without ranks, only the tier and id count, as the tables order.
struct TableEngine::Table {
    struct Row {
        std::uint32_t text{};     // Offset into texts.
        std::uint16_t length{};   // Bytes of text.
        std::uint8_t keys{};      // Bytes of code.
        std::array<char, 6> code{};
        std::uint64_t order{};
        std::string_view code_view() const { return {code.data(), keys}; }
    };
    std::string texts;
    std::vector<Row> rows;
    std::string_view text(const Row& row) const { return {texts.data() + row.text, row.length}; }
    // Every exact match is offered, as each character must stay typable.
    // Common exact matches (ranked by usage) come first, then common
    // completions, then the rare ones of both, so a box or a variant never
    // comes before a common character: by class (the rest of Big5-HKSCS
    // before characters outside it, compatibility forms, symbols), exact
    // matches before completions. Up to 50 are offered, or every exact
    // match, and each character once.
    std::vector<std::string> find(std::string_view keys) const {
        const auto by_code = [](const Row& row, std::string_view code) { return row.code_view() < code; };
        const auto begin = std::lower_bound(rows.begin(), rows.end(), keys, by_code);
        const auto end_key = std::string(keys) + '{'; // Keys are a to z; { sorts after them.
        const auto end = std::lower_bound(begin, rows.end(), std::string_view(end_key), by_code);
        auto exact = begin;
        while (exact != end && exact->code_view() == keys) {
            ++exact;
        }
        std::vector<std::string> result;
        std::unordered_set<std::string_view> offered;
        const auto offer = [&](const Row& row) {
            if (offered.insert(text(row)).second) {
                result.emplace_back(text(row));
            }
        };
        // Rare rows, each marked whether it is a completion, for the end.
        std::vector<std::pair<bool, const Row*>> rare;
        // Exact matches are in order already; the common ones come first.
        for (auto row = begin; row != exact; ++row) {
            if (common(*row)) {
                offer(*row);
            } else {
                rare.emplace_back(false, &*row);
            }
        }
        const auto exact_count = static_cast<std::size_t>(exact - begin);
        const auto wanted = std::min<std::size_t>(50 - std::min<std::size_t>(50, exact_count),
                                                  static_cast<std::size_t>(end - exact));
        if (wanted > 0) {
            // A short prefix spans thousands of rows: order an index of them,
            // never the table, and only as far as is offered, with room for
            // a character that has two longer codes.
            std::vector<const Row*> longer;
            longer.reserve(static_cast<std::size_t>(end - exact));
            for (auto row = exact; row != end; ++row) {
                longer.push_back(&*row);
            }
            const auto first = [](const Row* a, const Row* b) { return a->order < b->order; };
            const auto sorted = std::min(longer.size(), wanted + 16);
            std::partial_sort(longer.begin(), longer.begin() + static_cast<std::ptrdiff_t>(sorted), longer.end(),
                              first);
            std::unordered_set<std::string_view> taken;
            for (std::size_t i = 0; i < sorted && taken.size() < wanted; ++i) {
                if (!taken.insert(text(*longer[i])).second) {
                    continue;
                }
                if (common(*longer[i])) {
                    offer(*longer[i]);
                } else {
                    rare.emplace_back(true, longer[i]);
                }
            }
        }
        std::stable_sort(rare.begin(), rare.end(), [](const auto& a, const auto& b) {
            const auto ka = std::tuple{kind(*a.second), a.first, a.second->order};
            const auto kb = std::tuple{kind(*b.second), b.first, b.second->order};
            return ka < kb;
        });
        for (const auto& [completion, row] : rare) {
            offer(*row);
        }
        return result;
    }
    static std::uint64_t kind(const Row& row) { return row.order >> 44 & 0xF; }
    // Ranked by usage (chinese/char-ranks.txt). Without a rank list every row
    // counts as common, so the table's own order stands.
    static bool common(const Row& row) {
        return kind(row) == static_cast<std::uint64_t>(CharacterClass::Ranked);
    }
};

std::shared_ptr<const TableEngine::Table> TableEngine::load(const std::filesystem::path& file) {
    static std::map<std::pair<std::filesystem::path, std::filesystem::path>, std::weak_ptr<const Table>> loaded;
    static std::pair<std::filesystem::path, std::shared_ptr<const Ranks>> ranks_read;
    std::lock_guard lock(tables_mutex());
    const auto key = std::pair{file, ranks_file()};
    if (auto table = loaded[key].lock()) {
        return table;
    }
    if (!ranks_read.second || ranks_read.first != ranks_file()) {
        ranks_read = {ranks_file(), std::make_shared<const Ranks>(read_ranks(ranks_file()))};
    }
    const auto& ranks = *ranks_read.second;
    auto table = std::make_shared<Table>();
    Database database(file);
    const bool read = database.each([&](const char* code, const char* text, std::int64_t freq, std::int64_t id) {
        const std::string_view keys = code, characters = text;
        if (keys.empty() || keys.size() > 6 || characters.empty() || characters.size() > 0xFFFF ||
            !g_utf8_validate(text, static_cast<gssize>(characters.size()), nullptr)) {
            return;
        }
        Table::Row row;
        row.text = static_cast<std::uint32_t>(table->texts.size());
        row.length = static_cast<std::uint16_t>(characters.size());
        row.keys = static_cast<std::uint8_t>(keys.size());
        std::copy(keys.begin(), keys.end(), row.code.begin());
        // Higher frequency first; ids fit 24 bits, ranks 20 (about 25,000).
        const auto tier = static_cast<std::uint64_t>(0xFFFF - std::clamp<std::int64_t>(freq, 0, 0xFFFF));
        std::uint64_t place = 0;
        auto kind = CharacterClass::Ranked;
        if (!ranks.empty()) {
            kind = character_class(characters, ranks);
            if (kind == CharacterClass::Ranked || kind == CharacterClass::Listed) {
                const auto first = std::string(characters.substr(
                    0, static_cast<std::size_t>(g_utf8_next_char(text) - text)));
                place = std::min<std::uint64_t>(ranks.places.at(first), 0xFFFFF);
            }
        }
        row.order = tier << 48 | static_cast<std::uint64_t>(kind) << 44 | place << 24 |
                    std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max<std::int64_t>(id, 0)), 0xFFFFFF);
        table->texts += characters;
        table->rows.push_back(row);
    });
    if (!read || table->rows.empty()) {
        throw UiError(ui("ime.table-empty"));
    }
    std::sort(table->rows.begin(), table->rows.end(), [](const Table::Row& a, const Table::Row& b) {
        return a.code_view() != b.code_view() ? a.code_view() < b.code_view() : a.order < b.order;
    });
    table->texts.shrink_to_fit();
    table->rows.shrink_to_fit();
    loaded[key] = table;
    return table;
}
bool TableEngine::available(const std::filesystem::path& table) {
    std::error_code error;
    if (!sqlite().ready || !std::filesystem::is_regular_file(table, error)) {
        return false;
    }
    try {
        return Database(table).has_rows();
    } catch (const std::exception&) {
        return false;
    }
}
TableEngine::TableEngine(std::shared_ptr<const Table> table, std::size_t max_keys)
    : table_(std::move(table)), max_keys_(max_keys) {}
TableEngine::~TableEngine() = default;
void TableEngine::look_up() {
    candidates_ = keys_.empty() ? std::vector<std::string>{} : table_->find(keys_);
    selected_ = 0;
}
void TableEngine::type(char key) {
    if (key < 'a' || key > 'z' || keys_.size() >= max_keys_) {
        return;
    }
    if (static_cast<std::size_t>(g_utf8_strlen(chosen_.c_str(), -1)) >= retained_limit) {
        throw UiError(ui("ime.composition-full"));
    }
    keys_ += key;
    look_up();
}
void TableEngine::backspace() {
    if (!keys_.empty()) {
        keys_.pop_back();
        look_up();
    } else if (const char* previous =
                   g_utf8_find_prev_char(chosen_.c_str(), chosen_.c_str() + chosen_.size())) {
        chosen_.erase(static_cast<std::size_t>(previous - chosen_.c_str()));
    }
}
void TableEngine::cancel() {
    keys_.clear();
    chosen_.clear();
    candidates_.clear();
    selected_ = 0;
}
void TableEngine::cycle(int direction) {
    const int count = static_cast<int>(candidates_.size());
    if (count) {
        selected_ = (selected_ + direction + count) % count;
    }
}
void TableEngine::choose(int index) {
    if (index < 0 || index >= static_cast<int>(candidates_.size())) {
        return;
    }
    const auto& candidate = candidates_[static_cast<std::size_t>(index)];
    if (!g_utf8_validate(candidate.c_str(), static_cast<gssize>(candidate.size()), nullptr)) {
        throw UiError(ui("ime.table-invalid-text"));
    }
    const auto chosen = static_cast<std::size_t>(g_utf8_strlen(chosen_.c_str(), -1));
    const auto added = static_cast<std::size_t>(g_utf8_strlen(candidate.c_str(), -1));
    if (added > retained_limit - chosen) {
        throw UiError(ui("ime.composition-full"));
    }
    // A choice ends the reading. Accepted characters or phrases stay until
    // committed, and text the target refused goes out with the next choice.
    chosen_ += candidate;
    keys_.clear();
    candidates_.clear();
    selected_ = 0;
}
std::string TableEngine::preedit() const {
    std::string result = chosen_;
    for (const char key : keys_) {
        result += radicals[static_cast<std::size_t>(key - 'a')];
    }
    return result;
}
std::string TableEngine::text() const {
    if (keys_.empty()) {
        return chosen_;
    }
    return candidates_.empty() ? std::string{}
                               : chosen_ + candidates_[static_cast<std::size_t>(selected_)];
}
} // namespace framekeyboard

#include "table_engine.hpp"

#include <array>
#include <dlfcn.h>
#include <stdexcept>

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
    using Finalize = int (*)(void*);
    using Close = int (*)(void*);
    Open open{};
    Prepare prepare{};
    BindText bind{};
    Step step{};
    ColumnText column{};
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
                symbol(column, "sqlite3_column_text") && symbol(finalize, "sqlite3_finalize") &&
                symbol(close, "sqlite3_close");
    }
};
const Sqlite& sqlite() {
    static const Sqlite library;
    return library;
}
constexpr int sqlite_row = 100;
constexpr int sqlite_open_readonly = 1, sqlite_open_uri = 0x40;
const auto sqlite_transient = reinterpret_cast<void (*)(void*)>(-1);
} // namespace

std::optional<InputTable> table_for(const std::string& method) {
    const std::filesystem::path directory = "/usr/share/ibus-table/tables";
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
            throw std::runtime_error("Table input needs the SQLite library");
        }
        // Immutable: a system file that is never written, so no lock or journal.
        const auto uri = "file:" + table.string() + "?immutable=1";
        if (api.open(uri.c_str(), &handle, sqlite_open_readonly | sqlite_open_uri, nullptr) != 0) {
            release();
            throw std::runtime_error("The input table could not be opened");
        }
    }
    void release() {
        if (handle) {
            sqlite().close(handle);
            handle = nullptr;
        }
    }
    ~Database() { release(); }
    std::vector<std::string> find(const std::string& keys) {
        const auto& api = sqlite();
        // Exact matches first, then what the keys so far can still become. A
        // short prefix would match thousands, so those stop at enough to page
        // through; every exact match stays, or its character cannot be typed.
        void* query{};
        const char* text = "SELECT phrase FROM phrases WHERE tabkeys >= ?1 AND tabkeys < ?2 "
                           "ORDER BY tabkeys = ?1 DESC, freq DESC, id "
                           "LIMIT max(50, (SELECT count(*) FROM phrases WHERE tabkeys = ?1))";
        std::vector<std::string> result;
        if (api.prepare(handle, text, -1, &query, nullptr) != 0) {
            return result;
        }
        // Keys are a to z, so "{" sorts just after every code with this prefix.
        const auto end = keys + '{';
        api.bind(query, 1, keys.c_str(), -1, sqlite_transient);
        api.bind(query, 2, end.c_str(), -1, sqlite_transient);
        while (api.step(query) == sqlite_row) {
            if (const auto* phrase = api.column(query, 0)) {
                result.emplace_back(reinterpret_cast<const char*>(phrase));
            }
        }
        api.finalize(query);
        return result;
    }
};

bool TableEngine::available(const std::filesystem::path& table) {
    std::error_code error;
    if (!sqlite().ready || !std::filesystem::is_regular_file(table, error)) {
        return false;
    }
    try {
        return !Database(table).find("a").empty();
    } catch (const std::exception&) {
        return false;
    }
}
TableEngine::TableEngine(const std::filesystem::path& table, std::size_t max_keys)
    : database_(std::make_unique<Database>(table)), max_keys_(max_keys) {
    if (database_->find("a").empty()) {
        throw std::runtime_error("The input table is empty");
    }
}
TableEngine::~TableEngine() = default;
void TableEngine::look_up() {
    candidates_ = keys_.empty() ? std::vector<std::string>{} : database_->find(keys_);
    selected_ = 0;
}
void TableEngine::type(char key) {
    if (key < 'a' || key > 'z' || keys_.size() >= max_keys_) {
        return;
    }
    chosen_.clear();
    keys_ += key;
    look_up();
}
void TableEngine::backspace() {
    if (!keys_.empty()) {
        keys_.pop_back();
        look_up();
    } else {
        chosen_.clear();
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
    // Candidates are whole characters, so a choice ends the reading.
    chosen_ = candidates_[static_cast<std::size_t>(index)];
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
    return candidates_.empty() ? chosen_ : candidates_[static_cast<std::size_t>(selected_)];
}
} // namespace framekeyboard

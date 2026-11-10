#pragma once
#include "cjk_engine.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace framekeyboard {
// Cangjie and Quick look characters up in the tables SteamOS installs for
// IBus. Reading the system's file keeps it out of this project, as with Anthy.
struct InputTable {
    std::filesystem::path path;
    std::size_t max_keys{};
};
std::optional<InputTable> table_for(const std::string& method);

class TableEngine final : public CjkEngine {
  public:
    // Accepted choices remain local while output is unavailable. Keep that
    // backlog bounded independently of the transport's smaller commit size.
    static constexpr std::size_t retained_limit = 1024;
    // A table read whole and put in order: one per file and rank list in the
    // process, shared by every composer of it and never changed once loaded.
    struct Table;
    // Loads a table, or finds it already loaded. It takes its own lock, not
    // the engines' (CjkComposer), so reading a file holds up no typing.
    static std::shared_ptr<const Table> load(const std::filesystem::path& table);
    TableEngine(std::shared_ptr<const Table> table, std::size_t max_keys);
    ~TableEngine() override;
    // The table opens, holds characters, and SQLite is present.
    static bool available(const std::filesystem::path& table);
    void type(char key) override;
    void backspace() override;
    void cancel() override;
    void cycle(int direction) override;
    void choose(int index) override;
    std::string preedit() const override;
    std::string text() const override;
    std::vector<std::string> candidates() const override { return candidates_; }
    int selected() const override { return selected_; }
    bool empty() const override { return keys_.empty() && chosen_.empty(); }
    bool has_reading() const override { return !keys_.empty(); }
    bool chinese() const override { return true; }

  private:
    struct Database;
    void look_up();
    std::shared_ptr<const Table> table_;
    std::size_t max_keys_{};
    std::string keys_, chosen_;
    std::vector<std::string> candidates_;
    int selected_{};
};
} // namespace framekeyboard

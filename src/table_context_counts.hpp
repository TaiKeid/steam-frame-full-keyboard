#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <memory>
#include <vector>

namespace framekeyboard {
// Pinned public corpus counts, never personal text. The optional experiment
// reads sorted little-endian {three codepoints,count} records, zero first for
// bigrams. Invalid data leaves native ordering usable.
class TableContextCounts {
  public:
    static std::shared_ptr<const TableContextCounts> load(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (size <= 0 || size % 16 != 0 || size > 250000 * 16) {
            return {};
        }
        input.seekg(0);
        auto result = std::make_shared<TableContextCounts>();
        result->rows_.reserve(static_cast<std::size_t>(size) / 16);
        for (std::streamoff at = 0; at < size; at += 16) {
            std::array<unsigned char, 16> bytes{};
            if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
                return {};
            }
            const auto number = [&](std::size_t i) {
                return std::uint32_t(bytes[i]) | std::uint32_t(bytes[i + 1]) << 8 |
                       std::uint32_t(bytes[i + 2]) << 16 | std::uint32_t(bytes[i + 3]) << 24;
            };
            Row row{{number(0), number(4), number(8)}, std::log1p(number(12))};
            if (!number(12) || !g_unichar_validate(row.key[1]) || !row.key[1] ||
                !g_unichar_validate(row.key[2]) || !row.key[2] ||
                (row.key[0] && !g_unichar_validate(row.key[0])) ||
                (!result->rows_.empty() && row.key <= result->rows_.back().key)) {
                return {};
            }
            result->rows_.push_back(row);
        }
        return result;
    }
    double bonus(const std::string& previous, const std::string& candidate) const {
        if (previous.empty() || g_utf8_strlen(candidate.c_str(), -1) != 1) {
            return 0;
        }
        const auto count = g_utf8_strlen(previous.c_str(), -1);
        const auto last = g_utf8_get_char(g_utf8_offset_to_pointer(previous.c_str(), count - 1));
        const auto next = g_utf8_get_char(candidate.c_str());
        const auto bigram = find({0, last, next});
        return bigram + (count >= 2 ? .5 * find({g_utf8_get_char(previous.c_str()), last, next}) : 0);
    }

  private:
    struct Row {
        std::array<std::uint32_t, 3> key;
        double logarithm{};
    };
    std::vector<Row> rows_;
    double find(const std::array<std::uint32_t, 3>& key) const {
        const auto found =
            std::lower_bound(rows_.begin(), rows_.end(), key,
                             [](const Row& row, const auto& value) { return row.key < value; });
        return found != rows_.end() && found->key == key ? found->logarithm : 0;
    }
};
} // namespace framekeyboard

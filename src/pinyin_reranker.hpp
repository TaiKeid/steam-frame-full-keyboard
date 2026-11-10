#pragma once
// Fixed residual scoring shared by the separate Pinyin and Zhuyin experiments.
// Their model identities and feature contracts differ. Data stays external to
// the package; nothing is trained at runtime.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace framekeyboard {
class PinyinReranker {
  public:
    static constexpr std::size_t weight_count = 32768;
    static constexpr std::size_t wide_weight_count = 131072;
    static constexpr std::string_view magic = "FKPYR001";
    static constexpr std::string_view counted_magic = "FKPYR002";

    // Legacy files contain 32768 little-endian IEEE float32 weights. Counted
    // version 2 adds a little-endian uint32 size before the exact payload.
    // Invalid experiments leave normal input usable, never partially applied.
    static std::optional<PinyinReranker> load(const std::filesystem::path& path,
                                              std::string_view expected_magic = magic) {
        std::ifstream in(path, std::ios::binary);
        std::array<char, 8> header{};
        if (!in.read(header.data(), header.size())) {
            return std::nullopt;
        }
        std::size_t count = weight_count;
        const std::string_view version(header.data(), header.size());
        if (expected_magic == magic && version == counted_magic) {
            std::array<unsigned char, 4> bytes{};
            if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
                return std::nullopt;
            }
            count = std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
                    std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
            if (count != weight_count && count != wide_weight_count) {
                return std::nullopt; // Bound allocation and accept only the two validated sizes.
            }
        } else if (version != expected_magic) {
            return std::nullopt;
        }
        PinyinReranker result;
        result.weights_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::array<unsigned char, 4> bytes{};
            if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
                return std::nullopt;
            }
            const std::uint32_t bits = std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
                                       std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
            const float value = std::bit_cast<float>(bits);
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
            result.weights_.push_back(value);
        }
        if (in.peek() != std::char_traits<char>::eof()) {
            return std::nullopt;
        }
        return result;
    }

    // CRC32 matches the fixed research feature mapping, independent of the
    // standard library's implementation-specific string hash.
    static std::size_t feature_index(std::string_view value, std::size_t count = weight_count) {
        std::uint32_t crc = 0xffffffffU;
        for (const unsigned char byte : value) {
            crc = (crc >> 8) ^ crc_table_[(crc ^ byte) & 0xff];
        }
        return 32 + ((crc ^ 0xffffffffU) % (count - 32));
    }

    double score(std::string_view text, const std::vector<std::string_view>& words, std::size_t rank,
                 double native, double base, bool own = false) const {
        const std::string padded = "^" + std::string(text) + "$";
        std::vector<std::size_t> offsets;
        offsets.reserve(padded.size() + 1);
        for (std::size_t i = 0; i < padded.size(); ++i) {
            if ((static_cast<unsigned char>(padded[i]) & 0xc0) != 0x80) {
                offsets.push_back(i);
            }
        }
        offsets.push_back(padded.size());
        double result = native - base;
        for (std::size_t n = 1; n <= 3; ++n) {
            for (std::size_t i = 0; i + n < offsets.size(); ++i) {
                result += feature(std::to_string(n) + ":" +
                                  padded.substr(offsets[i], offsets[i + n] - offsets[i]));
            }
        }
        for (const auto word : words) {
            result += feature("word:" + std::string(word));
        }
        result += weights_[0] * static_cast<double>(rank) / 10.;
        result += weights_[1] * static_cast<double>(offsets.size() - 3) / 10.;
        result += weights_[2] * static_cast<double>(words.size()) / 10.;
        if (own) {
            result += weights_[3];
        }
        result += weights_[4] * (native - base) / 10.;
        return result;
    }

  private:
    inline static constexpr auto crc_table_ = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < table.size(); ++i) {
            auto value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
            }
            table[i] = value;
        }
        return table;
    }();
    std::vector<float> weights_;
    float feature(std::string_view value) const {
        return weights_[feature_index(value, weights_.size())];
    }
};
} // namespace framekeyboard

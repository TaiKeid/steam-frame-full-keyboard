#pragma once
#include "cjk_module.hpp"
#include <algorithm>
#include <opencc/Conversion.hpp>
#include <opencc/ConversionChain.hpp>
#include <opencc/Converter.hpp>
#include <opencc/Dict.hpp>
#include <opencc/Segmentation.hpp>
#include <opencc/Segments.hpp>
#include <optional>

namespace framekeyboard {
// Match the converter's nested group policies. Plain Dict::MatchPrefix on a
// group does not always apply its union policy or preserve the same tie order.
inline const opencc::DictEntry* contextual_match(const opencc::DictPtr& dict, std::string_view text) {
    if (const auto* children = dict->GetDictGroupItems()) {
        const opencc::DictEntry* best = nullptr;
        for (const auto& child : *children) {
            const auto* match = contextual_match(child, text);
            if (match && (!best || match->KeyLength() > best->KeyLength())) {
                best = match;
            }
            if (best && dict->GetMatchPolicy() == opencc::DictGroupMatchPolicy::ShortCircuit) {
                break;
            }
        }
        return best;
    }
    const auto match = dict->MatchPrefix(text.data(), text.size());
    return match.IsNull() ? nullptr : match.Get();
}

// Carry one byte boundary through actual replacement spans, never through a
// whole string's net length change. A length-changing phrase across the boundary
// has no safe per-character split, so the caller keeps independent conversion.
inline std::optional<std::size_t> converted_boundary(const opencc::ConversionPtr& conversion,
                                                     std::string_view input, std::size_t boundary,
                                                     const std::string& output) {
    if (boundary == 0 || boundary == input.size()) {
        return boundary == 0 ? 0 : output.size();
    }
    std::string checked;
    std::size_t mapped = 0;
    for (std::size_t at = 0; at < input.size();) {
        const auto rest = input.substr(at);
        const auto* match = contextual_match(conversion->GetDict(), rest);
        const auto length = match ? match->KeyLength() : utf8_prefix(rest, 1);
        if (length == 0 || length > rest.size()) {
            return std::nullopt;
        }
        const auto source = rest.substr(0, length);
        const auto replacement = match ? match->GetDefaultView() : source;
        if (boundary >= at + length) {
            mapped = checked.size() + replacement.size();
        } else if (boundary > at) {
            if (utf8_length(source) != utf8_length(replacement)) {
                return std::nullopt;
            }
            mapped =
                checked.size() + utf8_prefix(replacement, utf8_length(source.substr(0, boundary - at)));
        }
        checked += replacement;
        at += length;
    }
    // Also reject any unsupported matching behavior, such as an IDS sequence.
    return checked == output ? std::optional(mapped) : std::nullopt;
}

inline std::optional<std::string> contextual_suffix(const opencc::ConverterPtr& converter,
                                                    const std::string& prefix, const std::string& suffix,
                                                    const std::string& selected) {
    const auto text = prefix + suffix;
    // The pinned configuration normalizes compatibility ideographs before
    // segmentation. Keep the existing independent conversion for rare forms
    // rather than assume a boundary through that otherwise opaque pre-pass.
    for (std::size_t at = 0; at < text.size();) {
        const auto rest = std::string_view(text).substr(at);
        if (rare_character(rest)) {
            return std::nullopt;
        }
        at += utf8_prefix(rest, 1);
    }
    const auto segmentation = converter->GetSegmentation();
    const auto chain = converter->GetConversionChain();
    if (!segmentation || !chain) {
        return std::nullopt;
    }
    const auto conversions = chain->GetConversions();
    const auto segments = segmentation->Segment(text);
    std::string output;
    std::size_t remaining = prefix.size(), mapped = 0;
    for (const char* segment : *segments) {
        std::string current = segment;
        const auto consumed = std::min(remaining, current.size());
        auto boundary = consumed;
        for (const auto& conversion : conversions) {
            auto next = conversion->Convert(current);
            const auto moved = converted_boundary(conversion, current, boundary, next);
            if (!moved) {
                return std::nullopt;
            }
            boundary = *moved;
            current = std::move(next);
        }
        remaining -= consumed;
        mapped += boundary;
        output += current;
    }
    // The ordinary converter is authoritative, including any normalization.
    if (remaining != 0 || output != converter->Convert(text) || output.substr(0, mapped) != selected) {
        return std::nullopt;
    }
    return output.substr(mapped);
}
} // namespace framekeyboard

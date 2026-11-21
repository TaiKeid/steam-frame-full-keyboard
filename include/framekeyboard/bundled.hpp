#pragma once
// The resources built into the keyboard: every bundled layout, typing
// language, theme and interface language, and the emoji table. Each is kept
// zlib-compressed in the binary's read-only data (tools/embed_defaults.py)
// and unzipped only when it is used, so a text in use is the keyboard's own
// memory just as long as something keeps it (R436).
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace framekeyboard::bundled {
struct Resource {
    std::string_view kind; // "layouts", "languages", "themes", "locales" or "emoji"
    std::string_view name; // its file's name without the extension
    const unsigned char* data;
    std::size_t size;      // compressed
    std::size_t text_size; // unzipped, as the build read it
};
// An interface language's fields without its strings, uncompressed: the
// loader learns every language's tag, names and system codes from these,
// and unzips a language's texts only when it is chosen.
struct LocaleHead {
    std::string_view json;
    std::size_t resource; // its index in resources()
};
std::span<const Resource> resources();
std::span<const LocaleHead> locale_heads();
// The resource of that kind and name, or null.
const Resource* find(std::string_view kind, std::string_view name);
// The resource's text, unzipped. A resource that doesn't unzip to the size
// the build recorded is a damaged binary: std::runtime_error.
std::string text(const Resource& resource);
// For tests: record_unzips() starts recording afresh, and unzipped() then
// lists every resource text() unzipped, as "kind/name", in that order.
// Nothing is recorded until a test asks.
void record_unzips();
std::vector<std::string> unzipped();
} // namespace framekeyboard::bundled

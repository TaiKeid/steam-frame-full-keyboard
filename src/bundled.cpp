#include "framekeyboard/bundled.hpp"
#include <mutex>
#include <stdexcept>
#include <zlib.h>

namespace framekeyboard::bundled {
// Defined in the generated source (tools/embed_defaults.py).
extern const Resource resource_table[];
extern const std::size_t resource_count;
extern const LocaleHead locale_head_table[];
extern const std::size_t locale_head_count;
namespace {
// What text() unzipped, for tests; any thread may unzip, such as
// english_text()'s first call.
std::mutex record_mutex;
bool recording = false;
std::vector<std::string> record;
} // namespace

std::span<const Resource> resources() {
    return {resource_table, resource_count};
}
std::span<const LocaleHead> locale_heads() {
    return {locale_head_table, locale_head_count};
}
const Resource* find(std::string_view kind, std::string_view name) {
    for (const auto& resource : resources()) {
        if (resource.kind == kind && resource.name == name) {
            return &resource;
        }
    }
    return nullptr;
}
std::string text(const Resource& resource) {
    std::string result(resource.text_size, '\0');
    auto length = static_cast<uLongf>(result.size());
    // Exactly the recorded size, no more and no less, or the binary is damaged.
    if (uncompress(reinterpret_cast<Bytef*>(result.data()), &length, resource.data,
                   static_cast<uLong>(resource.size)) != Z_OK ||
        length != resource.text_size) {
        throw std::runtime_error("damaged bundled resource " + std::string(resource.kind) + "/" +
                                 std::string(resource.name));
    }
    if (const std::lock_guard lock(record_mutex); recording) {
        record.push_back(std::string(resource.kind) + "/" + std::string(resource.name));
    }
    return result;
}
std::vector<std::string> unzipped() {
    const std::lock_guard lock(record_mutex);
    return record;
}
void record_unzips() {
    const std::lock_guard lock(record_mutex);
    recording = true;
    record.clear();
}
} // namespace framekeyboard::bundled

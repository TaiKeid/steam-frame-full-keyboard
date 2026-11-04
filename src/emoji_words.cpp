#include "framekeyboard/emoji_words.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <glib-object.h>
#include <map>
#include <mutex>

namespace framekeyboard {
namespace {
constexpr const char* system_dictionary = "/usr/share/ibus/dicts/emoji-ja.dict";
// A katakana word in hiragana, as readings are typed; other characters stay.
std::string hiragana(const std::string& word) {
    std::string result;
    for (const char* p = word.c_str(); *p; p = g_utf8_next_char(p)) {
        auto c = g_utf8_get_char(p);
        if (c >= 0x30a1 && c <= 0x30f6) {
            c -= 0x60;
        }
        char buffer[8]{};
        result.append(buffer, static_cast<std::size_t>(g_unichar_to_utf8(c, buffer)));
    }
    return result;
}
struct Dictionary {
    std::mutex mutex;
    std::filesystem::path path{system_dictionary};
    bool loaded{};
    // A word's emoji, in the dictionary's order: rank 0 where the word is the
    // emoji's description, 1 where it is one of its keywords.
    std::map<std::string, std::vector<std::pair<int, std::string>>> emoji;
};
Dictionary& dictionary() {
    static Dictionary value;
    return value;
}
// Reads the dictionary through libibus, which stays loaded: it registers
// GLib types, which cannot be unloaded safely. Under the mutex.
void load(Dictionary& state) {
    state.loaded = true;
    std::error_code error;
    if (!std::filesystem::is_regular_file(state.path, error)) {
        return;
    }
    void* ibus = dlopen("libibus-1.0.so.5", RTLD_NOW | RTLD_LOCAL);
    if (!ibus) {
        return;
    }
    const auto type = reinterpret_cast<GType (*)()>(dlsym(ibus, "ibus_emoji_data_get_type"));
    const auto read = reinterpret_cast<GSList* (*)(const gchar*)>(dlsym(ibus, "ibus_emoji_data_load"));
    const auto emoji =
        reinterpret_cast<const gchar* (*)(gpointer)>(dlsym(ibus, "ibus_emoji_data_get_emoji"));
    const auto description =
        reinterpret_cast<const gchar* (*)(gpointer)>(dlsym(ibus, "ibus_emoji_data_get_description"));
    const auto keywords =
        reinterpret_cast<GSList* (*)(gpointer)>(dlsym(ibus, "ibus_emoji_data_get_annotations"));
    if (!type || !read || !emoji || !description || !keywords) {
        return;
    }
    type(); // The file's objects can only be read once their type exists.
    GSList* list = read(state.path.c_str());
    for (auto* item = list; item; item = item->next) {
        const gchar* text = emoji(item->data);
        auto add = [&](const gchar* word, int rank) {
            if (!text || !*text || !word || !*word) {
                return;
            }
            for (const auto& form : {std::string(word), hiragana(word)}) {
                auto& entries = state.emoji[form];
                if (std::none_of(entries.begin(), entries.end(),
                                 [&](const auto& entry) { return entry.second == text; })) {
                    entries.emplace_back(rank, text);
                }
            }
        };
        add(description(item->data), 0);
        for (auto* word = keywords(item->data); word; word = word->next) {
            add(static_cast<const gchar*>(word->data), 1);
        }
        g_object_unref(item->data);
    }
    g_slist_free(list);
    for (auto& [word, entries] : state.emoji) {
        std::stable_sort(entries.begin(), entries.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    }
}
} // namespace
std::vector<std::string> emoji_for(const std::vector<std::string>& words, std::size_t limit) {
    auto& state = dictionary();
    std::lock_guard lock(state.mutex);
    if (!state.loaded) {
        load(state);
    }
    // Every word's matches, the words' own order kept within a rank.
    std::vector<std::pair<int, std::string>> found;
    for (const auto& word : words) {
        if (const auto entries = state.emoji.find(word); entries != state.emoji.end()) {
            found.insert(found.end(), entries->second.begin(), entries->second.end());
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> result;
    for (const auto& [rank, text] : found) {
        if (result.size() >= limit) {
            break;
        }
        if (std::find(result.begin(), result.end(), text) == result.end()) {
            result.push_back(text);
        }
    }
    return result;
}
void set_emoji_dictionary(const std::filesystem::path& path) {
    auto& state = dictionary();
    std::lock_guard lock(state.mutex);
    state.path = path;
    state.loaded = false;
    state.emoji.clear();
}
} // namespace framekeyboard

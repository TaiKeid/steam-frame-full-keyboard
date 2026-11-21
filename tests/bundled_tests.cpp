// The bundled resources, compressed at build time and unzipped when used
// (R436): every one unzips to exactly what the build read, and a normal start
// unzips only English and the interface language in use.
//   bundled-tests SOURCE_DIR
#include "framekeyboard/app.hpp"
#include "framekeyboard/bundled.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <json-c/json.h>
#include <set>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Json {
    json_object* value;
    explicit Json(const std::string& text) : value(json_tokener_parse(text.c_str())) {}
    ~Json() { json_object_put(value); }
};
// Every resource in the table unzips to the repository's file: the same JSON,
// a locale without its `sources` section, and the emoji table byte for byte.
void every_resource_unzips(const fs::path& source) {
    std::set<std::string> seen;
    for (const auto& resource : bundled::resources()) {
        const auto text = bundled::text(resource);
        require(text.size() == resource.text_size, "a resource unzips to its recorded size");
        const std::string kind(resource.kind), name(resource.name);
        seen.insert(kind + "/" + name);
        if (kind == "emoji") {
            require(text == read(source / "emoji/emoji.tsv"), "the emoji table unzips byte for byte");
            continue;
        }
        Json unzipped(text), file(read(source / kind / (name + ".json")));
        require(unzipped.value && file.value, kind + "/" + name + ": both parse");
        if (kind == "locales") {
            require(!json_object_object_get(unzipped.value, "sources"),
                    name + ": no sources compiled in");
            json_object_object_del(file.value, "sources");
        }
        require(json_object_equal(unzipped.value, file.value), kind + "/" + name + ": the same JSON");
    }
    // And the table has every file the repository bundles.
    for (const char* kind : {"layouts", "languages", "themes", "locales"}) {
        for (const auto& entry : fs::directory_iterator(source / kind)) {
            if (entry.path().extension() == ".json") {
                require(seen.count(std::string(kind) + "/" + entry.path().stem().string()),
                        entry.path().string() + " is bundled");
            }
        }
    }
    // A damaged binary fails loudly: a size that doesn't match, or data cut short.
    const auto& sample = bundled::resources().front();
    for (const auto& damaged :
         {bundled::Resource{sample.kind, sample.name, sample.data, sample.size, sample.text_size + 1},
          bundled::Resource{sample.kind, sample.name, sample.data, sample.size / 2, sample.text_size}}) {
        bool thrown = false;
        try {
            bundled::text(damaged);
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        require(thrown, "a resource that doesn't unzip whole throws");
    }
    require(bundled::locale_heads().size() == 17, "a head for each of the 17 interface languages");
    for (const auto& head : bundled::locale_heads()) {
        Json parsed{std::string(head.json)};
        require(parsed.value && !json_object_object_get(parsed.value, "strings") &&
                    bundled::resources()[head.resource].kind == "locales",
                "a head is a language's fields without its strings");
    }
}
struct Sink : KeySink {
    void send(int, int) override {}
};
std::vector<std::string> unzipped_locales() {
    std::vector<std::string> result;
    for (const auto& name : bundled::unzipped()) {
        if (name.starts_with("locales/")) {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
// A normal start unzips English and the language in use, no other interface
// language and not the emoji table; a user file still replaces a bundled
// language's key; after the language in use changes, Reload config unzips
// the new one.
void unzips_only_what_is_used() {
    char folder[] = "/tmp/framekeyboard-bundled-tests-XXXXXX";
    require(mkdtemp(folder) != nullptr, "temporary folder");
    const fs::path config = folder;
    fs::create_directories(config / "locales");
    std::ofstream(config / "locales/uk.json")
        << R"json({"locale": "uk", "strings": {"tooltip.settings": "Налаштування (мої)"}})json";
    std::string posix = "uk_UA.UTF-8";
    Options options;
    options.config_dir = config;
    options.system_language = [&] { return SystemLanguage{"", posix}; };
    bundled::record_unzips();
    Sink sink;
    {
        App app(options, sink);
        app.tick(1);
        require(unzipped_locales() == std::vector<std::string>{"locales/en", "locales/uk"},
                "a start unzips English and the language in use only");
        const auto all = bundled::unzipped();
        require(std::find(all.begin(), all.end(), "emoji/emoji") == all.end(),
                "the emoji table waits for the picker");
        require(app.localization().text("tooltip.settings") == "Налаштування (мої)",
                "a user file replaces a bundled language's key");
        require(app.localization().text("card.general") == "Загальні",
                "and the rest comes from the bundled text");
        posix = "ja_JP.UTF-8";
        app.reload();
        const auto after = bundled::unzipped();
        require(app.localization().tag() == "ja" &&
                    std::count(after.begin(), after.end(), "locales/ja") == 1,
                "Reload config after the language changed unzips the new one");
    }
    fs::remove_all(config);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: bundled-tests SOURCE_DIR");
        every_resource_unzips(argv[1]);
        unzips_only_what_is_used();
        std::cout << "Bundled resources unzip whole, and only when used.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

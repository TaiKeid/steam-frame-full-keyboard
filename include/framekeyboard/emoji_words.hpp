#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace framekeyboard {
// Emoji for Japanese words, from the system's IBus emoji dictionary
// (docs/japanese.md, Emoji): its keywords and descriptions, such as 猫 or
// にっこり笑う猫. libibus reads it; both are loaded on first use, once per
// process, and nothing is offered without them.
//
// The emoji whose keyword or description is one of `words`, best first: a
// description equal to the word, then a keyword. Katakana words also match
// as hiragana. At most `limit`, each once.
std::vector<std::string> emoji_for(const std::vector<std::string>& words, std::size_t limit);
// Another dictionary than the system's, for the simulator and tests; before
// the first lookup.
void set_emoji_dictionary(const std::filesystem::path& dictionary);
} // namespace framekeyboard

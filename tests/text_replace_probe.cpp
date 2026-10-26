#include "framekeyboard/app.hpp"
#include "framekeyboard/ei_input.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
// Opt-in driver for a disposable receiver that the caller has focused and
// verifies before every command. Reads one command per line from stdin:
//   commit <text>       commit text through Gamescope, as a tap or swipe does
//   replace <n> <text>  delete n characters, then commit text, as picking a
//                       swipe alternative does
//   whole <text>        commit text as one request, even plain ASCII
//   paced <text>        commit text one acknowledged character at a time
// and prints "ok" or "fail" once Gamescope confirms or rejects it. Nothing is
// sent without a command. "\n" in text stands for a line break.
namespace {
std::string unescape(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            result += '\n';
            ++i;
        } else {
            result += text[i];
        }
    }
    return result;
}
} // namespace

int main() {
    EiSink sink("/run/user/" + std::to_string(getuid()) + "/gamescope-0-ei");
    for (int i = 0; i < 300 && !sink.text_available(); ++i) {
        sink.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!sink.text_available()) {
        std::cout << "unavailable" << std::endl;
        return 2;
    }
    // Gamescope's libei device may send no keymap; the sink then decides
    // per-character typing without one. Report which case this is.
    std::cerr << (sink.shortcut_code(XKB_KEY_a, -1) < 0 ? "no target keymap\n" : "target keymap\n");
    std::cout << "ready" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        std::size_t deletions = 0;
        if (command == "replace") {
            input >> deletions;
        }
        if (command == "whole" || command == "paced") {
            sink.force_per_character(command == "paced");
        } else if (command == "commit" || command == "replace") {
            sink.force_per_character({});
        } else {
            std::cout << "fail" << std::endl;
            continue;
        }
        std::string text;
        input.get(); // the one space before the text, which may itself start with spaces
        std::getline(input, text);
        text = unescape(text);
        // The first call submits; repeating the same call reports the result.
        bool done = false;
        for (int attempt = 0; attempt < 400 && !done; ++attempt) {
            done = deletions ? sink.replace_text(deletions, text) : sink.commit_text(text);
            if (!done) {
                sink.pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        std::cout << (done ? "ok" : "fail") << std::endl;
    }
    return 0;
}

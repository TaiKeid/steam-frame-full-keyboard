// The simulator's --chinese-candidates for a build without the simulator, such
// as the Frame's: what each Chinese engine offers for readings read from
// standard input (docs/simulator.md, Chinese candidates). Never sends input
// and learns nothing; for comparing the engines' answers, time and memory.
//
//   chinese-candidates-probe [--chinese-engine NAME|all]... [--chinese-method ID]...
//                            [--candidates N] < readings
#include "framekeyboard/chinese_candidates.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        framekeyboard::ChineseCandidatesOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + argument);
            }
            const std::string value = argv[++i];
            if (argument == "--chinese-engine") {
                if (value == "all") {
                    options.engines = {"pyzy", "libime", "mcbopomofo"};
                } else {
                    options.engines.push_back(value);
                }
            } else if (argument == "--chinese-method") {
                options.methods.push_back(value);
            } else if (argument == "--candidates") {
                options.count = std::stoul(value);
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }
        framekeyboard::report_chinese_candidates(std::cin, std::cout, options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "chinese-candidates-probe: " << error.what() << '\n';
        return 1;
    }
}

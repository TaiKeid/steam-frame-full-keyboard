#pragma once

#include "config.hpp"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace framekeyboard {
class LanguageMap;

// Fixed by the bundled FUTO Swipe encoder export.
constexpr int swipe_points = 64;
constexpr int swipe_max_keys = 64;

// Implemented by the optional engine module; tests supply fakes.
class SwipeEncoder {
  public:
    virtual ~SwipeEncoder() = default;
    // features: x row then y row, each swipe_points normalized values.
    // keys: swipe_max_keys (x, y) centers; mask marks the valid ones.
    // Returns log-probabilities, steps x (swipe_max_keys + 1), blank last.
    virtual std::vector<float> encode(const float* features, const float* keys,
                                      const unsigned char* mask, int& steps) = 0;
};
using SwipeEncoderFactory = std::function<std::unique_ptr<SwipeEncoder>()>;

struct SwipePoint {
    double x{}, y{}, t{}; // Layout coordinates; t in seconds.
};

// Letter keys of one layout/language pair. Each key's letter is its unshifted
// keymap character, so geometry and language stay independent.
struct SwipeKeyboard {
    std::vector<char32_t> letters;
    std::vector<SwipePoint> centers;
    std::map<std::string, int> key_index; // Layout key ID -> letter index.
    Rect frame;                           // Normalization frame around the letter block.
    double key_width{};
    // AltGr letters (Ukrainian ё on е) are swiped on the key that types them.
    std::map<char32_t, int> shifted_letters;
    // For a letter key outside the letter block (AZERTY é on the number row),
    // the base letter key a swipe may cross instead; -1 otherwise.
    std::vector<int> alternate;
    // A letter, its lowercase form, its key's AltGr letter or its accent-free base; -1 if none.
    int index_of(char32_t character) const;
};
std::optional<SwipeKeyboard> build_swipe_keyboard(const Layout& layout, const LanguageMap& keymap);

// Word list mapped onto one SwipeKeyboard as a trie of collapsed key paths.
class SwipeLexicon {
  public:
    // Lines are "word<TAB>f" with f the 0-255 log-frequency; # starts a comment.
    static SwipeLexicon parse(std::istream& input, const SwipeKeyboard& keyboard);
    static SwipeLexicon load(const fs::path& path, const SwipeKeyboard& keyboard);
    std::size_t size() const { return words_.size(); }
    struct Candidate {
        std::string word;
        double score{};
    };
    // CTC prefix beam search over the trie, then frequency/length rescoring.
    std::vector<Candidate> decode(const std::vector<float>& log_probs, int steps, int classes,
                                  std::size_t limit) const;

  private:
    struct Node {
        std::uint32_t first_child{}, child_count{}, first_word{}, word_count{};
    };
    struct Child {
        std::uint8_t key{};
        std::uint32_t node{};
    };
    std::vector<Node> nodes_;
    std::vector<Child> children_;
    std::vector<std::uint32_t> node_words_;
    std::vector<std::string> words_;
    std::vector<float> log_frequency_;
    std::vector<std::uint8_t> path_length_;
};

// Resample a path and normalize it into the encoder's letter-block frame.
std::vector<float> swipe_features(const std::vector<SwipePoint>& points, const SwipeKeyboard& keyboard);

// True when the bundled encoder module and model files are installed.
bool swipe_engine_installed(const fs::path& data_dir);
fs::path swipe_model_path(const fs::path& data_dir);
// User lexicons override bundled ones by ID; empty if neither exists.
fs::path swipe_lexicon_path(const fs::path& data_dir, const fs::path& config_dir, const std::string& id);
std::unique_ptr<SwipeEncoder> load_swipe_encoder(const fs::path& model);

// Owns the encoder and lexicon on one worker thread, so module loading,
// lexicon parsing and inference never run on the VR loop. Results carry the
// request ID; callers ignore IDs they have cancelled.
class SwipeService {
  public:
    explicit SwipeService(SwipeEncoderFactory factory);
    ~SwipeService();
    SwipeService(const SwipeService&) = delete;
    SwipeService& operator=(const SwipeService&) = delete;
    void prepare(std::shared_ptr<const SwipeKeyboard> keyboard, fs::path lexicon);
    void decode(std::uint64_t id, std::vector<SwipePoint> points);
    struct Result {
        std::uint64_t id{};
        std::vector<std::string> words;
        std::string error;
    };
    std::optional<Result> take_result();
    // Blocks until no request is outstanding, for input that must not overtake it.
    std::optional<Result> wait_result();

  private:
    void run();
    struct Job {
        std::shared_ptr<const SwipeKeyboard> keyboard;
        fs::path lexicon;
        std::uint64_t id{};
        std::vector<SwipePoint> points;
        bool prepare{};
    };
    SwipeEncoderFactory factory_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::vector<Job> jobs_;
    std::optional<Result> result_;
    std::size_t outstanding_{};
    bool stop_{};
    std::thread worker_;
};
} // namespace framekeyboard

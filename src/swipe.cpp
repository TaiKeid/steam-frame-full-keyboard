#include "framekeyboard/swipe.hpp"
#include "framekeyboard/input.hpp"

#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <fstream>
#include <glib.h>
#include <limits>
#include <malloc.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace framekeyboard {
namespace {
// Tuned on held-out FUTO swipes re-mapped onto these layouts with the bundled
// word lists (2026-10-23). Word lists store log-frequency as 0-255; about 13
// steps correspond to one natural-log unit of corpus frequency.
constexpr double frequency_weight = 0.8;
constexpr double length_weight = 1.0;
constexpr double frequency_scale = 13.0;
constexpr std::size_t beam_width = 64;
// Beams that survive to the end can still be implausible; keep alternatives
// within this log-score distance of the best word.
constexpr double candidate_margin = 10.0;
constexpr std::size_t max_words = 200000;
constexpr double negative = -1e30;

double log_add(double a, double b) {
    if (a < b) {
        std::swap(a, b);
    }
    return b <= negative ? a : a + std::log1p(std::exp(b - a));
}
bool apostrophe(char32_t c) {
    return c == U'\'' || c == U'’';
}
char32_t base_letter(char32_t c) {
    if (c == U'ß') {
        return U's'; // No decomposition; phones offer it on S.
    }
    gunichar decomposed[8];
    if (g_unichar_fully_decompose(c, FALSE, decomposed, 8) > 0) {
        return static_cast<char32_t>(g_unichar_tolower(decomposed[0]));
    }
    return c;
}
fs::path executable_dir() {
    return fs::read_symlink("/proc/self/exe").parent_path();
}
} // namespace
fs::path data_resource(const fs::path& data_dir, const fs::path& relative) {
    std::vector<fs::path> candidates;
    if (!data_dir.empty()) {
        candidates.push_back(data_dir / relative);
    }
    const auto exe = executable_dir();
    candidates.push_back(exe.parent_path() / "share/framekeyboard" / relative);
    candidates.push_back(exe / relative);
    for (const auto& path : candidates) {
        std::error_code error;
        if (fs::is_regular_file(path, error)) {
            return path;
        }
    }
    return {};
}
namespace {
fs::path module_path() {
    // Only sibling modules, as for the CJK engines: never a working-directory lookalike.
    const auto exe = executable_dir();
    auto path = exe / "engines/framekeyboard-swipe.so";
    if (!fs::exists(path)) {
        path = exe.parent_path() / "lib/framekeyboard/framekeyboard-swipe.so";
    }
    return path;
}
float interpolate(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
    if (x <= xs.front()) {
        return static_cast<float>(ys.front());
    }
    if (x >= xs.back()) {
        return static_cast<float>(ys.back());
    }
    const auto upper = std::upper_bound(xs.begin(), xs.end(), x);
    const auto i = static_cast<std::size_t>(upper - xs.begin());
    const double span = xs[i] - xs[i - 1];
    const double f = span > 0 ? (x - xs[i - 1]) / span : 0;
    return static_cast<float>(ys[i - 1] + (ys[i] - ys[i - 1]) * f);
}
// A letter's bounds grown by half the gap to the nearest key facing each
// side, so neighbouring letters' reaches meet in the middle of their gap. A
// side with no key facing it stays; a wide gap grows by at most a quarter key.
Rect swipe_reach(const Layout& layout, const Key& letter) {
    const auto& r = letter.bounds;
    constexpr double none = std::numeric_limits<double>::infinity();
    double left = none, right = none, top = none, bottom = none;
    for (const auto& key : layout.keys) {
        const auto& o = key.bounds;
        if (&key == &letter) {
            continue;
        }
        if (o.y < r.y + r.height && r.y < o.y + o.height) { // In its row.
            if (o.x + o.width <= r.x) {
                left = std::min(left, r.x - (o.x + o.width));
            } else if (o.x >= r.x + r.width) {
                right = std::min(right, o.x - (r.x + r.width));
            }
        }
        if (o.x < r.x + r.width && r.x < o.x + o.width) { // Above or below it.
            if (o.y + o.height <= r.y) {
                top = std::min(top, r.y - (o.y + o.height));
            } else if (o.y >= r.y + r.height) {
                bottom = std::min(bottom, o.y - (r.y + r.height));
            }
        }
    }
    const double most = std::min(r.width, r.height) / 4;
    auto grow = [most](double gap) { return gap == none ? 0 : std::min(gap / 2, most); };
    return {r.x - grow(left), r.y - grow(top), r.width + grow(left) + grow(right),
            r.height + grow(top) + grow(bottom)};
}
} // namespace

int SwipeKeyboard::index_of(char32_t character) const {
    auto find = [&](char32_t c) {
        const auto it = std::find(letters.begin(), letters.end(), c);
        return it == letters.end() ? -1 : static_cast<int>(it - letters.begin());
    };
    const auto lower = static_cast<char32_t>(g_unichar_tolower(character));
    if (const int index = find(lower); index >= 0) {
        return index;
    }
    if (const auto shifted = shifted_letters.find(lower); shifted != shifted_letters.end()) {
        return shifted->second;
    }
    // Letters typed through dead keys or on the number row are swiped on
    // their base key (é -> e, ß -> s).
    return find(base_letter(lower));
}

std::optional<SwipeKeyboard> build_swipe_keyboard(const Layout& layout, const LanguageMap& keymap) {
    SwipeKeyboard keyboard;
    double width = 0, height = 0;
    // Swipes are words in the letter rows, as on a phone, which has no number
    // row: keys above Q's row never swipe, and their letters (AZERTY é,
    // Italian ì, German ß, Russian ё) are swiped on the plain letter, as
    // FUTO's swipes were recorded.
    const auto q_key = std::find_if(layout.keys.begin(), layout.keys.end(),
                                    [](const Key& key) { return key.id == "KeyQ"; });
    const double letters_top =
        q_key != layout.keys.end() ? q_key->bounds.y : std::numeric_limits<double>::lowest();
    for (const auto& key : layout.keys) {
        if (key.action_kind != ActionKind::Key || key.bounds.y + key.bounds.height / 2 < letters_top ||
            keyboard.letters.size() >= static_cast<std::size_t>(swipe_max_keys)) {
            continue;
        }
        const auto character = xkb_keysym_to_utf32(keymap.symbol(key, {}, false, false));
        if (!character || !g_unichar_isalpha(character)) {
            continue; // Dead keys, digits and punctuation are not swipe letters.
        }
        const auto letter = static_cast<char32_t>(g_unichar_tolower(character));
        if (std::find(keyboard.letters.begin(), keyboard.letters.end(), letter) !=
            keyboard.letters.end()) {
            continue;
        }
        keyboard.key_index[key.id] = static_cast<int>(keyboard.letters.size());
        keyboard.letters.push_back(letter);
        keyboard.centers.push_back(
            {key.bounds.x + key.bounds.width / 2, key.bounds.y + key.bounds.height / 2});
        width += key.bounds.width;
        height += key.bounds.height;
    }
    if (keyboard.letters.size() < 10) {
        return {};
    }
    std::set<int> altgr;
    altgr.insert(key_code("AltRight"));
    for (const auto& [id, index] : keyboard.key_index) {
        const auto key = std::find_if(layout.keys.begin(), layout.keys.end(),
                                      [&](const Key& k) { return k.id == id; });
        const auto character = xkb_keysym_to_utf32(keymap.symbol(*key, altgr, false, false));
        const auto letter = static_cast<char32_t>(g_unichar_tolower(character));
        auto has = [&](char32_t c) {
            return std::find(keyboard.letters.begin(), keyboard.letters.end(), c) !=
                   keyboard.letters.end();
        };
        // An accented letter whose plain letter has a key folds to it instead,
        // as dead-key accents do and phone long-press accents train: Dutch ë
        // is AltGr+R on US international, but people swipe it through E.
        const auto base = base_letter(letter);
        if (character && g_unichar_isalpha(character) && !has(letter) &&
            !(base != letter && has(base))) {
            keyboard.shifted_letters.try_emplace(letter, index);
        }
    }
    const auto count = static_cast<double>(keyboard.letters.size());
    keyboard.key_width = width / count;
    // The encoder was trained on a letter-block frame and encodes key positions
    // with a cosine basis, which mirrors positions past the frame edge. Span
    // every letter key in the three main letter rows (ü, ñ, à beside them).
    // The rows are found by physical position, so Cyrillic layouts work the
    // same way. Measured on
    // held-out swipes: German 84.5% top-1 (83.7% for a-z only), Spanish unchanged.
    std::vector<SwipePoint> frame_centers;
    const auto q = keyboard.key_index.find("KeyQ"), z = keyboard.key_index.find("KeyZ");
    if (q != keyboard.key_index.end() && z != keyboard.key_index.end()) {
        const double row_top = keyboard.centers[static_cast<std::size_t>(q->second)].y;
        const double row_bottom = keyboard.centers[static_cast<std::size_t>(z->second)].y;
        for (const auto& center : keyboard.centers) {
            if (center.y >= row_top - 1 && center.y <= row_bottom + 1) {
                frame_centers.push_back(center);
            }
        }
    }
    if (frame_centers.size() < 20) {
        frame_centers = keyboard.centers; // Layouts without the usual letter rows.
    }
    double left = std::numeric_limits<double>::max(), top = left;
    double right = std::numeric_limits<double>::lowest(), bottom = right;
    for (const auto& c : frame_centers) {
        left = std::min(left, c.x);
        right = std::max(right, c.x);
        top = std::min(top, c.y);
        bottom = std::max(bottom, c.y);
    }
    const double half_width = keyboard.key_width / 2 + 3, half_height = height / count / 2 + 5;
    keyboard.frame = {left - half_width, top - half_height, right - left + 2 * half_width,
                      bottom - top + 2 * half_height};
    for (const auto& key : layout.keys) {
        if (keyboard.key_index.contains(key.id)) {
            keyboard.reach[key.id] = swipe_reach(layout, key);
        }
    }
    return keyboard;
}

SwipeLexicon SwipeLexicon::parse(std::istream& input, const SwipeKeyboard& keyboard) {
    SwipeLexicon lexicon;
    std::vector<std::vector<Child>> children(1);
    std::vector<std::vector<std::uint32_t>> words(1);
    std::set<std::string> respelled;
    std::string line;
    while (std::getline(input, line) && lexicon.words_.size() < max_words) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab > 64 ||
            !g_utf8_validate(line.data(), static_cast<gssize>(tab), nullptr)) {
            continue;
        }
        std::string word = line.substr(0, tab);
        const double frequency = std::clamp(std::atof(line.c_str() + tab + 1), 0.0, 255.0);
        if (!keyboard.spelling.empty()) {
            for (const auto& [from, to] : keyboard.spelling) {
                for (auto at = word.find(from); at != std::string::npos;
                     at = word.find(from, at + to.size())) {
                    word.replace(at, from.size(), to);
                }
            }
            // Two spellings can meet; the more frequent comes first in the list.
            if (!respelled.insert(word).second) {
                continue;
            }
        }
        std::vector<std::uint8_t> path;
        bool mapped = true;
        std::size_t length = 0;
        for (const char* p = word.c_str(); *p; p = g_utf8_next_char(p)) {
            const auto c = static_cast<char32_t>(g_utf8_get_char(p));
            if (apostrophe(c)) {
                continue;
            }
            const int index = keyboard.index_of(c);
            if (index < 0) {
                mapped = false;
                break;
            }
            ++length;
            // A swipe passes a doubled letter once; collapse repeats in the path.
            if (path.empty() || path.back() != index) {
                path.push_back(static_cast<std::uint8_t>(index));
            }
        }
        if (!mapped || length < 2) {
            continue;
        }
        auto insert = [&](const std::vector<std::uint8_t>& keys) {
            std::uint32_t node = 0;
            for (const auto key : keys) {
                auto& list = children[node];
                const auto it = std::find_if(list.begin(), list.end(),
                                             [&](const Child& child) { return child.key == key; });
                if (it != list.end()) {
                    node = it->node;
                    continue;
                }
                const auto created = static_cast<std::uint32_t>(children.size());
                list.push_back({key, created});
                children.emplace_back();
                words.emplace_back();
                node = created;
            }
            words[node].push_back(static_cast<std::uint32_t>(lexicon.words_.size()));
        };
        insert(path);
        lexicon.words_.push_back(word);
        lexicon.log_frequency_.push_back(static_cast<float>((frequency - 255) / frequency_scale));
        lexicon.path_length_.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(length, 255)));
    }
    // Flatten into contiguous arrays; the per-node vectors are construction-only.
    lexicon.nodes_.resize(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
        auto& node = lexicon.nodes_[i];
        node.first_child = static_cast<std::uint32_t>(lexicon.children_.size());
        node.child_count = static_cast<std::uint32_t>(children[i].size());
        lexicon.children_.insert(lexicon.children_.end(), children[i].begin(), children[i].end());
        node.first_word = static_cast<std::uint32_t>(lexicon.node_words_.size());
        node.word_count = static_cast<std::uint32_t>(words[i].size());
        lexicon.node_words_.insert(lexicon.node_words_.end(), words[i].begin(), words[i].end());
    }
    return lexicon;
}

SwipeLexicon SwipeLexicon::load(const fs::path& path, const SwipeKeyboard& keyboard) {
    std::ifstream input(path);
    if (!input) {
        throw UiError(ui("swipe.word-list-unavailable"));
    }
    auto lexicon = parse(input, keyboard);
    if (lexicon.size() == 0) {
        throw UiError(ui("swipe.word-list-empty"));
    }
    return lexicon;
}

std::vector<SwipeLexicon::Candidate> SwipeLexicon::decode(const std::vector<float>& log_probs, int steps,
                                                          int classes, std::size_t limit) const {
    struct Beam {
        std::uint32_t node{};
        double blank{negative}, symbol{negative};
        int last{-1};
        double total() const { return log_add(blank, symbol); }
    };
    if (nodes_.empty() || classes < 2 || steps <= 0 ||
        log_probs.size() < static_cast<std::size_t>(steps) * static_cast<std::size_t>(classes)) {
        return {};
    }
    const int blank = classes - 1;
    std::vector<Beam> beams{{0, 0.0, negative, -1}}, next;
    std::unordered_map<std::uint32_t, std::size_t> slot;
    for (int t = 0; t < steps; ++t) {
        const float* row =
            log_probs.data() + static_cast<std::size_t>(t) * static_cast<std::size_t>(classes);
        next.clear();
        slot.clear();
        auto add = [&](std::uint32_t node, double b, double s, int last) {
            auto [it, inserted] = slot.try_emplace(node, next.size());
            if (inserted) {
                next.push_back({node, b, s, last});
            } else {
                auto& beam = next[it->second];
                beam.blank = log_add(beam.blank, b);
                beam.symbol = log_add(beam.symbol, s);
            }
        };
        for (const auto& beam : beams) {
            const double total = beam.total();
            add(beam.node, total + row[blank], negative, beam.last);
            if (beam.last >= 0) {
                add(beam.node, negative, beam.symbol + row[beam.last], beam.last); // Repeats collapse.
            }
            const auto& node = nodes_[beam.node];
            for (std::uint32_t i = 0; i < node.child_count; ++i) {
                const auto& child = children_[node.first_child + i];
                if (child.key >= blank) {
                    continue;
                }
                const double p = row[child.key];
                // The same key twice in a path needs a blank between emissions.
                add(child.node, negative, (child.key == beam.last ? beam.blank : total) + p, child.key);
            }
        }
        const auto keep = std::min(beam_width, next.size());
        std::partial_sort(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(keep), next.end(),
                          [](const Beam& a, const Beam& b) { return a.total() > b.total(); });
        next.resize(keep);
        beams.swap(next);
    }
    std::vector<Candidate> result;
    std::unordered_map<std::uint32_t, std::size_t> seen; // A word can end two paths (é or e).
    for (const auto& beam : beams) {
        const auto& node = nodes_[beam.node];
        for (std::uint32_t i = 0; i < node.word_count; ++i) {
            const auto word = node_words_[node.first_word + i];
            const double score = beam.total() + frequency_weight * log_frequency_[word] +
                                 length_weight * path_length_[word];
            if (const auto [it, inserted] = seen.try_emplace(word, result.size()); !inserted) {
                result[it->second].score = std::max(result[it->second].score, score);
                continue;
            }
            result.push_back({words_[word], score});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::erase_if(result,
                  [&](const Candidate& c) { return c.score < result.front().score - candidate_margin; });
    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

std::vector<float> swipe_features(const std::vector<SwipePoint>& points, const SwipeKeyboard& keyboard) {
    std::vector<float> features(2 * swipe_points);
    if (points.empty()) {
        return features;
    }
    std::vector<double> t, x, y;
    for (const auto& p : points) {
        // Ignore samples that go back in time; intervals must be monotonic.
        const double ms = (p.t - points.front().t) * 1000;
        if (!t.empty() && ms < t.back()) {
            continue;
        }
        t.push_back(ms);
        x.push_back((p.x - keyboard.frame.x) / keyboard.frame.width);
        y.push_back((p.y - keyboard.frame.y) / keyboard.frame.height);
    }
    // As in the encoder's training pipeline: resample uniformly at 60 Hz, then
    // to a fixed number of evenly spaced samples.
    if (t.back() > 1e-3) {
        const auto n60 = std::max<std::size_t>(
            2, static_cast<std::size_t>(std::lround(t.back() / (1000.0 / 60))) + 1);
        std::vector<double> rx(n60), ry(n60), tt(n60);
        for (std::size_t i = 0; i < n60; ++i) {
            tt[i] = t.back() * static_cast<double>(i) / static_cast<double>(n60 - 1);
            rx[i] = interpolate(t, x, tt[i]);
            ry[i] = interpolate(t, y, tt[i]);
        }
        x.swap(rx);
        y.swap(ry);
    }
    std::vector<double> index(x.size());
    for (std::size_t i = 0; i < index.size(); ++i) {
        index[i] = static_cast<double>(i);
    }
    for (int i = 0; i < swipe_points; ++i) {
        const double at = static_cast<double>(x.size() - 1) * i / (swipe_points - 1);
        features[static_cast<std::size_t>(i)] = interpolate(index, x, at);
        features[static_cast<std::size_t>(swipe_points + i)] = interpolate(index, y, at);
    }
    return features;
}

fs::path swipe_model_path(const fs::path& data_dir) {
    return data_resource(data_dir, "swipe/futo-swipe/model_fp32.pte");
}
bool swipe_engine_installed(const fs::path& data_dir) {
    std::error_code error;
    return !swipe_model_path(data_dir).empty() && fs::is_regular_file(module_path(), error);
}
fs::path swipe_lexicon_path(const fs::path& data_dir, const fs::path& config_dir,
                            const std::string& id) {
    const fs::path relative = fs::path("swipe/lexicons") / (id + ".tsv");
    std::error_code error;
    if (!config_dir.empty() && fs::is_regular_file(config_dir / relative, error)) {
        return config_dir / relative;
    }
    return data_resource(data_dir, relative);
}

namespace {
using CreateSwipeEncoder = SwipeEncoder* (*)(const char*);
struct SwipeModule {
    void* handle{};
    CreateSwipeEncoder create{};
    SwipeModule() {
        // The pinned backend initializes process-global CPU metadata at load
        // time, but its deinitializers do not release that storage. Reuse one
        // mapped image instead of orphaning those globals on each reload.
        // ModuleEncoder still destroys every encoder and model instance.
        handle = dlopen(module_path().c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (!handle) {
            throw UiError(ui("swipe.engine-unavailable"));
        }
        create =
            reinterpret_cast<CreateSwipeEncoder>(dlsym(handle, "framekeyboard_create_swipe_encoder_v1"));
        if (!create) {
            dlclose(handle);
            throw UiError(ui("swipe.engine-incompatible"));
        }
    }
    ~SwipeModule() { dlclose(handle); }
};
class ModuleEncoder final : public SwipeEncoder {
  public:
    ModuleEncoder(std::shared_ptr<SwipeModule> module, std::unique_ptr<SwipeEncoder> encoder)
        : module_(std::move(module)), encoder_(std::move(encoder)) {}
    std::vector<float> encode(const float* features, const float* keys, const unsigned char* mask,
                              int& steps) override {
        return encoder_->encode(features, keys, mask, steps);
    }

  private:
    std::shared_ptr<SwipeModule> module_;
    std::unique_ptr<SwipeEncoder> encoder_; // Destroyed before the module that owns its code.
};
} // namespace

std::unique_ptr<SwipeEncoder> load_swipe_encoder(const fs::path& model) {
    if (model.empty()) {
        throw UiError(ui("swipe.model-unavailable"));
    }
    auto module = std::make_shared<SwipeModule>();
    // Own the engine before allocating its wrapper. If that allocation fails,
    // destroy the engine while the local module still owns its code.
    std::unique_ptr<SwipeEncoder> encoder(module->create(model.c_str()));
    if (!encoder) {
        throw UiError(ui("swipe.model-failed"));
    }
    return std::make_unique<ModuleEncoder>(std::move(module), std::move(encoder));
}

SwipeService::SwipeService(SwipeEncoderFactory factory)
    : factory_(std::move(factory)), worker_([this] { run(); }) {
}
SwipeService::~SwipeService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}
void SwipeService::prepare(std::shared_ptr<const SwipeKeyboard> keyboard, fs::path lexicon) {
    {
        std::lock_guard lock(mutex_);
        // Only the newest keyboard matters; drop queued preparation for older ones.
        std::erase_if(jobs_, [](const Job& job) { return job.prepare; });
        jobs_.push_back({std::move(keyboard), std::move(lexicon), 0, {}, true, false, ++requested_});
    }
    wake_.notify_all();
}
void SwipeService::release(bool encoder) {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(jobs_, [](const Job& job) { return job.prepare; });
        Job job;
        job.release = true;
        job.release_encoder = encoder;
        jobs_.push_back(std::move(job));
    }
    wake_.notify_all();
}
std::size_t SwipeService::loads() {
    std::lock_guard lock(mutex_);
    return loads_;
}
void SwipeService::decode(std::uint64_t id, std::vector<SwipePoint> points) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({{}, {}, id, std::move(points), false});
        ++outstanding_;
    }
    wake_.notify_all();
}
std::optional<SwipeService::Result> SwipeService::take_result() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}
bool SwipeService::ready() {
    std::lock_guard lock(mutex_);
    return prepared_ == requested_;
}
std::optional<SwipeService::Result> SwipeService::wait_result() {
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return outstanding_ == 0; });
    return std::exchange(result_, std::nullopt);
}
void SwipeService::run() {
    std::unique_ptr<SwipeEncoder> encoder;
    ErrorText encoder_error;
    bool encoder_tried = false;
    std::shared_ptr<const SwipeKeyboard> keyboard;
    std::optional<SwipeLexicon> lexicon;
    const auto not_ready = error_text(ui("swipe.not-ready"));
    auto lexicon_error = not_ready;
    // Identifies the loaded word list file, so an unchanged one is not parsed again.
    fs::path loaded_path;
    fs::file_time_type loaded_time{};
    std::uintmax_t loaded_size{};
    auto return_memory = [] {
#ifdef __GLIBC__
        malloc_trim(0); // Parsing frees about as much as the trie keeps.
#endif
    };
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.erase(jobs_.begin());
        }
        if (job.release) {
            keyboard.reset();
            lexicon.reset();
            lexicon_error = not_ready;
            loaded_path.clear();
            if (job.release_encoder) {
                encoder.reset();
                encoder_error = {};
                encoder_tried = false; // The next prepare loads it again.
            }
            return_memory();
            continue;
        }
        if (job.prepare) {
            std::error_code stat_error;
            const auto time = fs::last_write_time(job.lexicon, stat_error);
            const auto size = stat_error ? 0 : fs::file_size(job.lexicon, stat_error);
            if (lexicon && keyboard && *keyboard == *job.keyboard && job.lexicon == loaded_path &&
                !stat_error && time == loaded_time && size == loaded_size) {
                std::lock_guard lock(mutex_);
                prepared_ = job.generation;
                continue;
            }
            if (!encoder_tried) {
                encoder_tried = true;
                try {
                    encoder = factory_();
                } catch (const std::exception& error) {
                    encoder_error = error_text(error);
                }
            }
            keyboard = std::move(job.keyboard);
            lexicon.reset();
            loaded_path.clear();
            try {
                lexicon = SwipeLexicon::load(job.lexicon, *keyboard);
                lexicon_error = {};
                if (!stat_error) {
                    loaded_path = job.lexicon;
                    loaded_time = time;
                    loaded_size = size;
                }
            } catch (const std::exception& error) {
                lexicon_error = error_text(error);
            }
            return_memory();
            std::lock_guard lock(mutex_);
            prepared_ = job.generation;
            ++loads_;
            continue;
        }
        Result result{job.id, {}, {}};
        if (!encoder) {
            result.error =
                encoder_error.empty() ? error_text(ui("swipe.engine-unavailable")) : encoder_error;
        } else if (!lexicon || !keyboard) {
            result.error = lexicon_error;
        } else {
            try {
                std::vector<float> keys(2 * swipe_max_keys);
                std::vector<unsigned char> mask(swipe_max_keys);
                for (std::size_t i = 0; i < keyboard->centers.size(); ++i) {
                    keys[2 * i] = static_cast<float>((keyboard->centers[i].x - keyboard->frame.x) /
                                                     keyboard->frame.width);
                    keys[2 * i + 1] = static_cast<float>((keyboard->centers[i].y - keyboard->frame.y) /
                                                         keyboard->frame.height);
                    mask[i] = 1;
                }
                const auto features = swipe_features(job.points, *keyboard);
                int steps = 0;
                const auto log_probs = encoder->encode(features.data(), keys.data(), mask.data(), steps);
                for (const auto& candidate : lexicon->decode(log_probs, steps, swipe_max_keys + 1, 5)) {
                    result.words.push_back(candidate.word);
                }
            } catch (const std::exception& error) {
                result.error = error_text(error);
            }
        }
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(result);
            --outstanding_;
        }
        done_.notify_all();
    }
}
} // namespace framekeyboard

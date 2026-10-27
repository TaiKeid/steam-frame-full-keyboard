#include "framekeyboard/shell_host.hpp"

#include "framekeyboard/app.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <unistd.h>

namespace framekeyboard {
std::optional<Transform> json_matrix(json_object* value) {
    if (!value || !json_object_is_type(value, json_type_array)) {
        return {};
    }
    std::vector<double> numbers;
    auto take = [&](json_object* item) {
        if (!json_object_is_type(item, json_type_double) && !json_object_is_type(item, json_type_int)) {
            return false;
        }
        numbers.push_back(json_object_get_double(item));
        return std::isfinite(numbers.back());
    };
    for (std::size_t i = 0; i < json_object_array_length(value); ++i) {
        auto* item = json_object_array_get_idx(value, i);
        if (json_object_is_type(item, json_type_array)) {
            for (std::size_t j = 0; j < json_object_array_length(item); ++j) {
                if (!take(json_object_array_get_idx(item, j))) {
                    return {};
                }
            }
        } else if (!take(item)) {
            return {};
        }
    }
    if (numbers.size() != 12) {
        return {};
    }
    Transform transform{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            transform[row][col] = numbers[row * 4 + col];
        }
    }
    return transform;
}

json_object* matrix_json(const Transform& transform) {
    auto* rows = json_object_new_array();
    for (const auto& row : transform) {
        auto* values = json_object_new_array();
        for (const double value : row) {
            json_object_array_add(values, json_object_new_double(value));
        }
        json_object_array_add(rows, values);
    }
    return rows;
}

json_object* rect_uv(const Rect& rect) {
    auto* object = json_object_new_object();
    json_object_object_add(object, "u", json_object_new_double(rect.x / panel_width));
    json_object_object_add(object, "v",
                           json_object_new_double((rect.y + popup_margin) / texture_height));
    json_object_object_add(object, "width", json_object_new_double(rect.width / panel_width));
    json_object_object_add(object, "height", json_object_new_double(rect.height / texture_height));
    return object;
}

std::pair<double, double> uv_to_panel(double u, double v) {
    return {u * panel_width, v * texture_height - popup_margin};
}

const char* update_state_name(Updates::State state) {
    switch (state) {
    case Updates::State::Checking:
        return "checking";
    case Updates::State::Latest:
        return "latest";
    case Updates::State::Available:
        return "available";
    case Updates::State::Installing:
        return "downloading";
    case Updates::State::Prepared:
        return "prepared";
    case Updates::State::Error:
        return "error";
    case Updates::State::Idle:
        break;
    }
    return "idle";
}

std::string update_error_code(const std::string& reason) {
    if (reason == "network-unavailable" || reason == "server-unavailable" ||
        reason == "download-timeout") {
        return "network";
    }
    if (reason == "rate-limit") {
        return "rate_limit";
    }
    if (reason == "checksum-failed") {
        return "checksum";
    }
    if (reason == "install-busy") {
        return "locked";
    }
    if (reason == "disk-full" || reason == "permission-denied" || reason == "files-unavailable") {
        return "disk";
    }
    if (reason == "incompatible") {
        return "incompatible";
    }
    if (reason == "prepared-expired" || reason == "connection-lost" || reason == "helper-unavailable") {
        return "expired";
    }
    if (reason == "invalid-release" || reason == "download-failed" || reason == "version-mismatch" ||
        reason == "validation-failed" || reason == "wrong-platform" ||
        reason == "not-installed-release") {
        return "invalid_package";
    }
    return "failed";
}

PreviewRenderer::~PreviewRenderer() {
    if (worker_.joinable()) {
        worker_.join();
    }
    // Previews last only as long as the keyboard.
    std::error_code ignored;
    for (int i = 0; i < 4; ++i) {
        std::filesystem::remove(directory_ / ("preview-" + std::to_string(i) + ".png"), ignored);
    }
}

bool PreviewRenderer::start(Request request) {
    if (busy_.exchange(true)) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread([this, request = std::move(request)] { run(request); });
    return true;
}

std::optional<PreviewRenderer::Result> PreviewRenderer::take() {
    std::lock_guard lock(mutex_);
    auto result = std::move(result_);
    result_.reset();
    return result;
}

void PreviewRenderer::run(Request request) {
    Result result;
    result.id = request.id;
    std::error_code ignored;
    // A throwaway configuration: sample state only, never the user's own.
    const auto config = directory_ / (".preview-" + std::to_string(getpid()));
    try {
        std::filesystem::remove_all(config, ignored);
        std::filesystem::create_directories(config);
        std::filesystem::permissions(config, std::filesystem::perms::owner_all);
        std::ofstream(config / "config.json")
            << R"({"schema_version":1,"active":{"layout":")" << request.layout << R"(","language":")"
            << request.language << R"(","theme":")" << request.theme << R"("},"dictation":"off"})";
        if (!request.theme_json.empty()) {
            // The shell's theme as a user theme of this throwaway configuration.
            auto theme = json_ptr(json_tokener_parse(request.theme_json.c_str()));
            if (!theme || !json_object_is_type(theme.get(), json_type_object)) {
                throw std::runtime_error("invalid shell theme");
            }
            json_object_object_add(theme.get(), "id", json_object_new_string(request.theme.c_str()));
            json_object_object_add(theme.get(), "name", json_object_new_string("Shell theme"));
            std::filesystem::create_directories(config / "themes");
            std::ofstream(config / "themes" / (request.theme + ".json"))
                << json_object_to_json_string_ext(theme.get(), JSON_C_TO_STRING_PLAIN);
        }
        Options options;
        options.config_dir = config;
        options.data_dir = request.data_dir;
        options.mode = request.view == "settings"    ? Mode::RenderSettings
                       : request.view == "emoji"     ? Mode::RenderEmoji
                       : request.view == "dictation" ? Mode::RenderDictation
                                                     : Mode::Render;
        NullSink sink;
        App app(options, sink);
        if (app.selection().layout != request.layout || app.selection().language != request.language ||
            app.selection().theme != request.theme) {
            throw std::runtime_error("unknown or incompatible profiles");
        }
        if (options.mode == Mode::RenderSettings) {
            app.show_settings();
        } else if (options.mode == Mode::RenderEmoji) {
            app.show_emoji();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!app.emoji_settled() && std::chrono::steady_clock::now() < deadline) {
                app.tick(monotonic_seconds());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        } else if (options.mode == Mode::RenderDictation) {
            app.show_dictation();
        }
        app.tick(monotonic_seconds());
        app.paint(monotonic_seconds());
        // At most four previews are kept; the oldest is overwritten.
        const auto path = directory_ / ("preview-" + std::to_string(next_ % 4) + ".png");
        ++next_;
        if (request.width > 0 && request.width != panel_width) {
            // Scaled to the width asked for, keeping the texture's proportions.
            const double scale = static_cast<double>(request.width) / panel_width;
            const int height = std::max(1, static_cast<int>(std::lround(texture_height * scale)));
            cairo_surface_t* scaled =
                cairo_image_surface_create(CAIRO_FORMAT_ARGB32, request.width, height);
            cairo_t* cr = cairo_create(scaled);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, app.renderer.surface(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_destroy(cr);
            const auto status = cairo_surface_write_to_png(scaled, path.c_str());
            cairo_surface_destroy(scaled);
            if (status != CAIRO_STATUS_SUCCESS) {
                throw std::runtime_error("could not write the preview");
            }
        } else {
            app.renderer.write_png(path.string());
        }
        result.path = path.string();
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    std::filesystem::remove_all(config, ignored);
    {
        std::lock_guard lock(mutex_);
        result_ = std::move(result);
    }
    busy_ = false;
}
} // namespace framekeyboard

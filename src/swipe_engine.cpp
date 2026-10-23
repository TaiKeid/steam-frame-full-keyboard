// Optional module: the FUTO Swipe encoder through ExecuTorch with XNNPACK.
// Loaded on demand by SwipeService's worker thread; never by the VR loop.
#include "framekeyboard/swipe.hpp"

#include <executorch/extension/module/module.h>
#include <executorch/extension/tensor/tensor.h>
#include <executorch/extension/threadpool/threadpool.h>

#include <stdexcept>

namespace {
using executorch::aten::ScalarType;
using executorch::extension::make_tensor_ptr;
using executorch::extension::Module;

class FutoEncoder final : public framekeyboard::SwipeEncoder {
  public:
    explicit FutoEncoder(const char* model) : module_(model, Module::LoadMode::Mmap) {
        // One inference is about a millisecond; a single thread avoids waking
        // every core for each word.
        if (auto* pool = executorch::extension::threadpool::get_threadpool()) {
            pool->_unsafe_reset_threadpool(1);
        }
        if (module_.load_forward() != executorch::runtime::Error::Ok) {
            throw std::runtime_error("cannot load swipe model");
        }
    }
    std::vector<float> encode(const float* features, const float* keys, const unsigned char* mask,
                              int& steps) override {
        using framekeyboard::swipe_max_keys;
        using framekeyboard::swipe_points;
        auto input = make_tensor_ptr({1, 2, swipe_points},
                                     std::vector<float>(features, features + 2 * swipe_points));
        auto centers =
            make_tensor_ptr({1, swipe_max_keys, 2}, std::vector<float>(keys, keys + 2 * swipe_max_keys));
        auto valid =
            make_tensor_ptr({1, swipe_max_keys}, std::vector<std::uint8_t>(mask, mask + swipe_max_keys),
                            {}, {}, ScalarType::Bool);
        auto result = module_.forward({input, centers, valid});
        if (!result.ok() || result->empty() || !result->at(0).isTensor()) {
            throw std::runtime_error("swipe model inference failed");
        }
        const auto output = result->at(0).toTensor(); // [1, steps, keys + blank]
        if (output.dim() != 3 || output.size(2) != swipe_max_keys + 1 ||
            output.scalar_type() != ScalarType::Float) {
            throw std::runtime_error("unexpected swipe model output");
        }
        steps = static_cast<int>(output.size(1));
        const float* data = output.const_data_ptr<float>();
        return {data, data + output.numel()};
    }

  private:
    Module module_;
};
} // namespace

extern "C" __attribute__((visibility("default"))) framekeyboard::SwipeEncoder*
framekeyboard_create_swipe_encoder_v1(const char* model) {
    try {
        return new FutoEncoder(model);
    } catch (...) {
        return nullptr; // Exceptions never cross the module boundary.
    }
}

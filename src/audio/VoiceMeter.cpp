#include "VoiceMeter.h"
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>

namespace Caesura {
namespace {
std::atomic<uint64_t> lastGeneration{0};

uint64_t nextGeneration() noexcept {
    auto previous = lastGeneration.load(std::memory_order_relaxed);
    while (previous != (std::numeric_limits<uint64_t>::max)()) {
        if (lastGeneration.compare_exchange_weak(previous, previous + 1,
                std::memory_order_relaxed)) return previous + 1;
    }
    return 0; // Exhaustion is permanent; never wrap/reuse an identity.
}

void increment(uint64_t& value) noexcept {
    if (value != (std::numeric_limits<uint64_t>::max)()) ++value;
}
}

class VoiceMeter::Instance final : public SoLoud::FilterInstance {
public:
    explicit Instance(VoiceMeter& meter) : m_meter(meter) {
        increment(m_meter.state.instancesCreated);
    }
    ~Instance() override { increment(m_meter.state.instancesDestroyed); }
    void filter(float* buffer, unsigned samples, unsigned stride,
                unsigned channels, float, SoLoud::time) override {
        m_meter.observe(buffer, samples, stride, channels);
    }
private:
    VoiceMeter& m_meter;
};

SoLoud::FilterInstance* VoiceMeter::createInstance() {
    // SoLoud calls this factory under its non-recursive mutex. Do not allow
    // allocation failure to unwind through vendor code with the mutex held.
    return new (std::nothrow) Instance(*this);
}

void VoiceMeter::invalidate() noexcept {
    const auto generation = nextGeneration();
    state.available = generation != 0;
    if (generation) state.generation = generation;
    state.sampledGeneration = 0;
    state.rawRms = 0;
    state.valid = false;
}

void VoiceMeter::observe(const float* buffer, unsigned samples,
                         unsigned stride, unsigned channels) noexcept {
    increment(state.blockSerial);
    state.rawRms = 0;
    state.valid = false;
    state.sampledGeneration = state.generation;
    if (!buffer || !samples || !channels || stride < samples ||
        size_t(channels) > (std::numeric_limits<size_t>::max)() / stride) {
        increment(state.invalidBlocks);
        return;
    }
    double squares = 0;
    for (unsigned channel = 0; channel < channels; ++channel) {
        for (unsigned frame = 0; frame < samples; ++frame) {
            const double sample = buffer[size_t(channel) * stride + frame];
            if (!std::isfinite(sample)) {
                increment(state.invalidBlocks);
                return;
            }
            squares += sample * sample;
        }
    }
    const double rms = std::sqrt(squares / (double(samples) * channels));
    if (!std::isfinite(rms)) {
        increment(state.invalidBlocks);
        return;
    }
    state.rawRms = rms;
    state.valid = state.available && state.generation != 0;
}
} // namespace Caesura

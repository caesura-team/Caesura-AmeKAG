// Opt-in muted physical-backend clock characterization. No manual mix calls.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <soloud.h>
#include <soloud_wav.h>
#include "audio/SoLoudAudioEngine.h"
#include "di/BackendRegistry.h"
#include "di/api/ThreadAssert.h"
#include <nlohmann_json.hpp>
#include <windows.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void observeProduction(nlohmann::json& report, const std::string& path) {
    using namespace Caesura;
    using Clock = std::chrono::steady_clock;
    detail::g_mainThreadId = std::this_thread::get_id();
    SoLoudAudioEngine owner(SoLoudAudioEngine::OutputMode::Device);
    auto& registry = BackendRegistry::instance();
    struct RestoreRegistration {
        IAudioBackend* previous;
        ~RestoreRegistration() { BackendRegistry::instance().setAudioBackend(previous); }
    } restore{registry.getAudioBackend()};
    registry.setAudioBackend(&owner);
    auto* audio = registry.getAudioBackend();
    audio->setGlobalVolume(0);
    if (!audio->init()) throw std::runtime_error("Production Device init failed");
    auto* native = dynamic_cast<SoLoudAudioEngine*>(audio);
    report["backend"] = {{"name", native->soloud().getBackendString()},
        {"id", native->soloud().getBackendId()}, {"sample_rate", native->soloud().getBackendSamplerate()},
        {"reported_buffer_size", native->soloud().getBackendBufferSize()}};
    if (native->soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER)
        throw std::runtime_error("Actual production Device backend required");
    const auto handle = audio->playVoice(path);
    if (!handle) throw std::runtime_error("Production voice admission failed");
    report["admitted_handle"] = handle;
    const auto start = Clock::now();
    unsigned completions = 0;
    double highest = 0;
    while (std::chrono::duration<double>(Clock::now() - start).count() < 2) {
        const auto stream = native->soloud().getStreamTime(handle);
        if (stream > highest) highest = stream;
        audio->update(0);
        completions += audio->consumeVoiceCompletions();
        if (completions) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto observed = audio->getSnapshot();
    report["wall_seconds"] = std::chrono::duration<double>(Clock::now() - start).count();
    report["max_voice_seconds"] = highest;
    report["natural_completions"] = completions;
    report["session_handles"] = observed.sessionHandles;
    report["live_voices"] = observed.liveVoices;
    report["paused_at_boundary"] = native->soloud().getPause(handle);
    report["voice_valid_at_boundary"] = native->soloud().isValidVoiceHandle(handle);
    if (completions != 1 || observed.sessionHandles || observed.liveVoices)
        throw std::runtime_error("Production 80 ms voice did not naturally complete and reclaim within two seconds");
    if (audio->consumeVoiceCompletions() != 0)
        throw std::runtime_error("Natural completion was duplicated");
}

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    using json = nlohmann::json;
    if (argc != 3 && argc != 4) return 2;
    json report = {{"kind", "DEVICE_CLOCK_OBSERVATION"}, {"pid", GetCurrentProcessId()},
                   {"physical_audibility", "NOT_MEASURED"}, {"manual_mix_calls", 0}};
    try {
        if (argc == 4 && std::string(argv[1]) == "production") {
            report["kind"] = "PRODUCTION_DEVICE_CLOCK_REGRESSION";
            observeProduction(report, argv[3]);
        } else {
        if (argc != 3) throw std::runtime_error("Invalid characterization arguments");
        const unsigned requested = unsigned(std::stoul(argv[1]));
        if (requested != 0 && requested != 2) throw std::runtime_error("Unknown buffer experiment");
        SoLoud::Soloud mixer;
        const auto init = mixer.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::AUTO,
                                    SoLoud::Soloud::AUTO, requested, 2);
        if (init != SoLoud::SO_NO_ERROR) throw std::runtime_error("Device init failed");
        mixer.setGlobalVolume(0);
        report["backend"] = {{"name", mixer.getBackendString()}, {"id", mixer.getBackendId()},
            {"sample_rate", mixer.getBackendSamplerate()}, {"reported_buffer_size", mixer.getBackendBufferSize()},
            {"requested_buffer", requested}};
        if (mixer.getBackendId() == SoLoud::Soloud::NULLDRIVER)
            throw std::runtime_error("Actual device backend required");
        std::vector<float> samples(3840, 0); // Fixed 80 ms at 48 kHz; muted silence.
        SoLoud::Wav wave;
        if (wave.loadRawWave(samples.data(), unsigned(samples.size()), 48000, 1, true, false)
            != SoLoud::SO_NO_ERROR) throw std::runtime_error("PCM fixture failed");
        const auto voice = mixer.play(wave);
        if (!voice || !mixer.isValidVoiceHandle(voice)) throw std::runtime_error("Admission failed");
        report["admitted_handle"] = voice;
        const auto start = Clock::now();
        double highest = 0;
        bool ended = false;
        while (std::chrono::duration<double>(Clock::now() - start).count() < 2) {
            const auto streamTime = mixer.getStreamTime(voice);
            if (streamTime > highest) highest = streamTime;
            if (!mixer.isValidVoiceHandle(voice)) { ended = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        report["wall_seconds"] = std::chrono::duration<double>(Clock::now() - start).count();
        report["max_voice_seconds"] = highest;
        report["naturally_ended"] = ended;
        report["paused_at_boundary"] = mixer.getPause(voice);
        report["voice_valid_at_boundary"] = mixer.isValidVoiceHandle(voice);
        // Only after the observation boundary; cleanup is never natural finish.
        mixer.stopAll();
        mixer.deinit();
        }
        report["status"] = "OBSERVED";
    } catch (const std::exception& e) {
        report["status"] = "FAIL";
        report["error"] = e.what();
    }
    std::ofstream output(argv[2], std::ios::binary);
    output << report.dump(2) << '\n';
    output.close();
    std::cout << report.dump() << '\n';
    return output && report["status"] == "OBSERVED" ? 0 : 1;
}

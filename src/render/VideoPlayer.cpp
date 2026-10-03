#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <memory>
#include <chrono>
#include <cerrno>
#define PL_MPEG_IMPLEMENTATION
#include "../../external/pl_mpeg/pl_mpeg.h"
#include "VideoPlayer.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include "../di/BackendRegistry.h"
#include "../audio/api/IAudioBackend.h"

#ifdef CAESURA_VIDEO_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
}
#endif

namespace Caesura {

#ifdef CAESURA_VIDEO_FFMPEG
static std::shared_ptr<AVChannelLayout> copyChannelLayout(const AVChannelLayout& source) {
    auto layout = std::shared_ptr<AVChannelLayout>(new AVChannelLayout{},
        [](AVChannelLayout* value) {
            av_channel_layout_uninit(value);
            delete value;
        });
    if (av_channel_layout_copy(layout.get(), &source) < 0) return {};
    return layout;
}
#endif

// pl_mpeg audio callback: appends decoded interleaved float PCM to the
// owning VideoState queue. High-level plm_decode (also used by seek) invokes
// this callback; the worker's direct plm_decode_audio call forwards its
// returned samples explicitly. Both paths copy PCM before decoding again.
static void onPlmAudio(plm_t* plm, plm_samples_t* samples, void* user) {
    auto* self = static_cast<VideoPlayer*>(user);
    self->onAudioDecoded(plm, samples);
}

VideoPlayer::VideoPlayer() {
    BackendRegistry::instance().registerDeviceLostListener(this);
}

VideoPlayer::~VideoPlayer() {
    BackendRegistry::instance().unregisterDeviceLostListener(this);
    shutdown();
}

void VideoPlayer::onDeviceLost() {
    // All video textures are GPU-owned; invalidate them so the update loop
    // re-uploads the next decoded frame (no stale-handle submits).
    std::lock_guard<std::mutex> lock(m_audioMutex);
    for (auto& [id, vs] : m_videos) {
        vs->texture = BGFX_INVALID_HANDLE;
    }
}

struct VideoPlayer::MemoryInput {
    std::vector<uint8_t> bytes;
    size_t position = 0;
    unsigned secondaryRefusals = 0;
    bool opening = true;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
};

VideoHandle VideoPlayer::open(const char* path) {
    return path && *path ? openImpl(path, {}) : VideoHandle{};
}

VideoHandle VideoPlayer::openMemory(std::vector<uint8_t> bytes) {
    m_memoryIoRefusals = 0;
    constexpr size_t maxBytes = 64u * 1024u * 1024u;
    if (bytes.empty() || bytes.size() > maxBytes || m_videos.size() >= 4) return {};
    auto input = std::make_shared<MemoryInput>();
    input->bytes = std::move(bytes);
    const auto result = openImpl("<asset-memory>", input);
    input->opening = false;
    m_memoryIoRefusals = input->secondaryRefusals;
    return result;
}

VideoHandle VideoPlayer::openImpl(const char* path, std::shared_ptr<MemoryInput> memory) {
    if (m_nextId == 0 || m_nextId == UINT32_MAX) return {}; // Never reuse stale handles.
    auto state = std::make_shared<VideoState>();
    auto& vs = *state;
    vs.memory = std::move(memory);
    struct Guard {
        VideoPlayer* player; VideoState* state; bool committed = false;
        ~Guard() { if (!committed) player->releaseState(*state); }
    } guard{this, &vs};
#ifdef CAESURA_VIDEO_FFMPEG
    vs.useFFmpeg = true;
    AVFormatContext* format = avformat_alloc_context();
    if (!format) return {};
    vs.avFormat = format;
    if (vs.memory) {
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        if (!buffer) return {};
        auto* io = avio_alloc_context(buffer, 32768, 0, vs.memory.get(),
            [](void* opaque, uint8_t* dst, int wanted) -> int {
                auto& in = *static_cast<MemoryInput*>(opaque);
                if (wanted <= 0) return AVERROR(EINVAL);
                const size_t count = std::min(static_cast<size_t>(wanted), in.bytes.size() - in.position);
                if (!count) return AVERROR_EOF;
                std::memcpy(dst, in.bytes.data() + in.position, count); in.position += count;
                return static_cast<int>(count);
            }, nullptr,
            [](void* opaque, int64_t offset, int whence) -> int64_t {
                auto& in = *static_cast<MemoryInput*>(opaque);
                if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return static_cast<int64_t>(in.bytes.size());
                whence &= ~AVSEEK_FORCE;
                int64_t base = 0;
                if (whence == SEEK_CUR) base = static_cast<int64_t>(in.position);
                else if (whence == SEEK_END) base = static_cast<int64_t>(in.bytes.size());
                else if (whence != SEEK_SET) return AVERROR(EINVAL);
                // Range comparison precedes addition to avoid signed overflow.
                if (offset < -base || offset > static_cast<int64_t>(in.bytes.size()) - base) return AVERROR(EINVAL);
                in.position = static_cast<size_t>(base + offset); return static_cast<int64_t>(in.position);
            });
        if (!io) { av_free(buffer); return {}; }
        vs.customIo = std::shared_ptr<void>(io, [](void* value) {
            auto* owned = static_cast<AVIOContext*>(value);
            av_freep(&owned->buffer); avio_context_free(&owned);
        });
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->opaque = vs.memory.get();
        format->io_open = [](AVFormatContext* parent, AVIOContext**, const char*, int, AVDictionary**) -> int {
            if (parent->opaque) ++static_cast<MemoryInput*>(parent->opaque)->secondaryRefusals;
            return AVERROR(EACCES); // No filename/URL, including nested demuxer resources.
        };
        format->interrupt_callback.callback = [](void* opaque) -> int {
            const auto& input = *static_cast<MemoryInput*>(opaque);
            return input.opening && std::chrono::steady_clock::now() >= input.deadline;
        };
        format->interrupt_callback.opaque = vs.memory.get();
        format->max_streams = 32;
        format->probesize = 8 * 1024 * 1024;
        format->max_analyze_duration = 5 * AV_TIME_BASE;
        // Defense in depth for protocol accesses not routed through io_open.
        format->protocol_whitelist = av_strdup("");
        if (!format->protocol_whitelist) return {};
    }
    int result = avformat_open_input(&format, vs.memory ? nullptr : path, nullptr, nullptr);
    // open_input may free/reset its supplied context on failure.
    vs.avFormat = format;
    if (result < 0 || !format) return {};
    if (avformat_find_stream_info(format, nullptr) < 0) return {};
    if (vs.memory && vs.memory->secondaryRefusals) return {};
    int videoIndex = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (videoIndex < 0 && format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) videoIndex = static_cast<int>(i);
        if (vs.audioStreamIndex < 0 && format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) vs.audioStreamIndex = static_cast<int>(i);
    }
    if (videoIndex < 0) return {};
    vs.videoStreamIndex = videoIndex;
    auto* stream = format->streams[videoIndex];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) return {};
    auto* codec = avcodec_alloc_context3(decoder); vs.avCodec = codec;
    if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0) return {};
    if (vs.memory) codec->max_pixels = 16 * 1024 * 1024;
    codec->thread_count = vs.memory ? 2 : 0;
    if (avcodec_open2(codec, decoder, nullptr) < 0) return {};
    vs.width = codec->width; vs.height = codec->height;
    if (vs.width <= 0 || vs.height <= 0 || vs.width > 16384 || vs.height > 16384
        || (vs.memory && uint64_t(vs.width) * uint64_t(vs.height) > 16u * 1024u * 1024u)) return {};
    vs.duration = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0;
    vs.frameRate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0 ? av_q2d(stream->avg_frame_rate)
        : (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0 ? av_q2d(stream->r_frame_rate) : 30.0);
    if (vs.audioStreamIndex >= 0) {
        const auto* parameters = format->streams[vs.audioStreamIndex]->codecpar;
        const auto* audioDecoder = avcodec_find_decoder(parameters->codec_id);
        auto* audio = audioDecoder ? avcodec_alloc_context3(audioDecoder) : nullptr;
        vs.avAudioCodec = audio;
        if (audio && avcodec_parameters_to_context(audio, parameters) >= 0 && avcodec_open2(audio, audioDecoder, nullptr) >= 0) {
            vs.avAudioFrame = av_frame_alloc();
            vs.sampleRate = audio->sample_rate > 0 ? audio->sample_rate : 44100;
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            SwrContext* resampler = nullptr;
            auto layout = copyChannelLayout(audio->ch_layout);
            if (vs.avAudioFrame && layout && swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT,
                audio->sample_rate, &audio->ch_layout, audio->sample_fmt, audio->sample_rate, 0, nullptr) >= 0
                && swr_init(resampler) >= 0) {
                vs.swrCtx = resampler; vs.expectedSampleFmt = audio->sample_fmt;
                vs.expectedSampleRate = audio->sample_rate; vs.expectedChLayout = std::move(layout); vs.audioEnabled = true;
            } else if (resampler) swr_free(&resampler);
        }
        if (!vs.audioEnabled) vs.audioStreamIndex = -1;
    }
    vs.swsCtx = sws_getContext(vs.width, vs.height, codec->pix_fmt, vs.width, vs.height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    vs.avFrame = av_frame_alloc(); vs.avFrameRGB = av_frame_alloc();
    if (!vs.swsCtx || !vs.avFrame || !vs.avFrameRGB) return {};
    const int rgbSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, vs.width, vs.height, 1);
    if (rgbSize <= 0) return {};
    vs.rgbaBuffer.resize(static_cast<size_t>(rgbSize));
    auto* rgb = static_cast<AVFrame*>(vs.avFrameRGB);
    if (av_image_fill_arrays(rgb->data, rgb->linesize, vs.rgbaBuffer.data(), AV_PIX_FMT_RGBA, vs.width, vs.height, 1) < 0) return {};
#else
    vs.useFFmpeg = false;
    auto* plm = vs.memory ? plm_create_with_memory(vs.memory->bytes.data(), vs.memory->bytes.size(), 0)
                         : plm_create_with_filename(path);
    vs.plm = plm;
    if (!plm) return {};
    vs.width = plm_get_width(plm); vs.height = plm_get_height(plm);
    if (vs.width <= 0 || vs.height <= 0 || vs.width > 16384 || vs.height > 16384
        || (vs.memory && uint64_t(vs.width) * uint64_t(vs.height) > 16u * 1024u * 1024u)) return {};
    vs.duration = plm_get_duration(plm); vs.frameRate = plm_get_framerate(plm);
    if (vs.frameRate <= 0) vs.frameRate = 30;
    vs.audioEnabled = true; vs.sampleRate = plm_get_samplerate(plm);
    if (vs.sampleRate > 0) { plm_set_audio_enabled(plm, 1); plm_set_audio_decode_callback(plm, onPlmAudio, this); }
#endif
    vs.texture = bgfx::createTexture2D(static_cast<uint16_t>(vs.width), static_cast<uint16_t>(vs.height),
        false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT);
    if (!bgfx::isValid(vs.texture)) return {};
    const VideoHandle handle{m_nextId++};
    // Keep this strong owner until commit: emplace may construct its node and
    // then fail allocating rehash buckets. Its temporary node would otherwise
    // destroy VideoState before the raw-pointer cleanup guard unwinds.
    m_videos.emplace(handle.id, state);
    guard.committed = true;
    DEBUG_INFO(SubSys::Render, ErrCode::Ok, "VideoPlayer: opened '%s' %dx%d (id=%u)", path, vs.width, vs.height, handle.id);
    return handle;
}

double VideoPlayer::clampSeekTime(double time, double duration) {
    // The bound is deliberately FFmpeg-independent (AV_TIME_BASE lives behind
    // the CAESURA_VIDEO_FFMPEG guard) and far above any real media duration.
    constexpr double kMaxSeekSeconds = 1e6;  // ~11.6 days
    if (!std::isfinite(time) || time <= 0.0 || time > kMaxSeekSeconds) return 0.0;
    if (duration > 0.0 && time > duration) return duration;
    return time;
}

VideoPlayer::DrainPlan VideoPlayer::planDrain(size_t chunkFloats,
                                            size_t framesPerChunk,
                                            bool playFailed) {
    DrainPlan plan;
    if (framesPerChunk == 0) return plan;
    const size_t floatsPerChunk = framesPerChunk * 2;  // stereo
    if (floatsPerChunk == 0) return plan;
    plan.chunks = chunkFloats / floatsPerChunk;
    plan.dropRemainder = playFailed;
    return plan;
}

void VideoPlayer::drainAudio(VideoState& vs) {
    if (!vs.audioEnabled || vs.sampleRate <= 0) return;
    std::vector<float> chunk;
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (vs.audioQueue.empty()) return;
        chunk.swap(vs.audioQueue);
    }
    auto* audio = BackendRegistry::instance().getAudioBackend();
    if (!audio) return;  // no backend: nothing could play it; chunk is dropped
    // Roughly one chunk per second of audio: 2 channels x sampleRate frames.
    // The chunk-count math lives in planDrain so tests and production share
    // one source of truth.
    const unsigned int numFrames = static_cast<unsigned int>(vs.sampleRate);
    const DrainPlan plan = planDrain(chunk.size(), numFrames, /*playFailed=*/false);
    unsigned int offset = 0;
    bool playFailed = false;
    for (size_t n = 0; n < plan.chunks; ++n) {
        const unsigned int h = audio->playRawPCM(
            chunk.data() + offset, numFrames,
            static_cast<unsigned int>(vs.sampleRate), 2);
        if (h == 0) { playFailed = true; break; }
        vs.audioHandle = h;
        vs.audioHandles.push_back(h);
        if (vs.audioHandles.size() > 4) {
            // Cap concurrent chunks: stop the oldest so a long video cannot
            // stack an unbounded number of overlapping 1-second voices.
            audio->stopSEHandle(vs.audioHandles.front());
            vs.audioHandles.erase(vs.audioHandles.begin());
        }
        vs.audioStarted = true;
        if (vs.volume != 1.0f) audio->setSEVolume(h, vs.volume);
        offset += numFrames * 2;
    }
    if (offset < chunk.size()) {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (playFailed) {
            // Backend unavailable / budget exhausted: drop the remainder
            // (requeueing would grow audioQueue unboundedly). The 8MB cap
            // in onAudioDecoded bounds the producer side too.
        } else {
            // Not enough data for a full second yet: keep it for the next
            // drain (dropping here would make pl_mpeg audio permanently
            // silent, since each update decodes only a few frames).
            // Copy the remainder first: inserting from chunk into the same
            // underlying vector via begin() iterators is formally UB.
            const std::vector<float> remainder(chunk.begin() + offset, chunk.end());
            vs.audioQueue.insert(vs.audioQueue.begin(),
                                remainder.begin(), remainder.end());
        }
    }
}

// Locking invariant: onAudioDecoded and the FFmpeg worker lambda (both
// capture `this` for m_audioMutex) touch audioQueue/m_videos under
// m_audioMutex; open()/close() mutate the map without it. This is safe
// because update() blocks in m_jobSystem->waitIdle() before the main thread
// touches the map, serializing worker vs. main. Keep that ordering if new
// asynchronous call sites are added.
void VideoPlayer::onAudioDecoded(void* plmRaw, void* samplesRaw) {
    auto* samples = static_cast<plm_samples_t*>(samplesRaw);
    if (!samples) return;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    for (auto& kv : m_videos) {
        if (kv.second->plm == plmRaw) {
            // Interleaved float PCM: samples->count * 2 (stereo interleave).
            // Cap the queue: if the consumer (drainAudio) is failing or the
            // audio backend is absent, drop audio instead of growing forever.
            if (kv.second->audioQueue.size() < 8 * 1024 * 1024 / sizeof(float)) {
                kv.second->audioQueue.insert(kv.second->audioQueue.end(),
                    samples->interleaved,
                    samples->interleaved + samples->count * 2);
            }
            return;
        }
    }
}
void VideoPlayer::setLoop(VideoHandle handle, bool loop) {
    auto vs = find(handle);
    if (!vs) return;
    if (vs->useFFmpeg) {
        // FFmpeg path: rewind is done inside update() (av_seek_frame to 0 on EOF).
        vs->loop = loop;
    } else if (vs->plm) {
        plm_set_loop(static_cast<plm_t*>(vs->plm), loop ? 1 : 0);
    }
}

void VideoPlayer::setVolume(VideoHandle handle, float volume) {
    auto vs = find(handle);
    if (!vs) return;
    if (!std::isfinite(volume)) return;  // NaN/Inf: reject (never clamp to a valid value)
    vs->volume = (volume < 0.0f) ? 0.0f : (volume > 1.5f ? 1.5f : volume);
    auto* audio = BackendRegistry::instance().getAudioBackend();
    if (audio) {
        for (const auto h : vs->audioHandles) audio->setSEVolume(h, vs->volume);
    }
}

void VideoPlayer::close(VideoHandle handle) {
    auto shared = find(handle);
    if (!shared) return;
    VideoState& vs = *shared;

    auto* audio = BackendRegistry::instance().getAudioBackend();
    if (audio) {
        for (const auto h : vs.audioHandles) audio->stopSEHandle(h);
        vs.audioHandles.clear();
        vs.audioHandle = 0;
        vs.audioStarted = false;
        { std::lock_guard<std::mutex> lk(m_audioMutex); vs.audioQueue.clear(); }
    }
    vs.playing = false;
    vs.ended   = true;
    vs.closing = true;

    // Defer the destructive erase to updateAll() (RD-1): a decoder worker
    // submitted in the same frame may still hold the VideoState pointer,
    // and destroying it here would use-after-free. Logical teardown above
    // is complete and immediate; resource release is one frame late at most.
    if (std::find(m_pendingClose.begin(), m_pendingClose.end(), handle.id)
            == m_pendingClose.end()) {
        m_pendingClose.push_back(handle.id);
    }
}

void VideoPlayer::closeAll() {
    std::vector<uint32_t> ids;
    ids.reserve(m_videos.size());
    for (const auto& [id, state] : m_videos) {
        (void)state;
        ids.push_back(id);
    }
    for (const auto id : ids) close(VideoHandle{id});
}

bool VideoPlayer::update(VideoHandle handle, double dt) {
    (void)dt;
    auto vs = find(handle);
    if (!vs || !vs->playing || vs->ended) return false;

    if (vs->useFFmpeg) {
        // Frame-rate pacing: advance the playhead by dt (same policy as the pl_mpeg path).
        if (dt > 0.0 && vs->frameRate > 0.0) vs->playhead += dt;
#ifdef CAESURA_VIDEO_FFMPEG
        // Offload read+decode+convert to JobSystem worker.
        // State is exclusive: main thread waits before touching FFmpeg objects.
        auto frame = std::make_shared<DecodedFrame>();
        frame->width  = vs->width;
        frame->height = vs->height;

        m_jobSystem->submit(
            [frame, vs, this]() {
                auto* fmt  = static_cast<AVFormatContext*>(vs->avFormat);
                auto* cc   = static_cast<AVCodecContext*>(vs->avCodec);
                auto* f    = static_cast<AVFrame*>(vs->avFrame);
                auto* fRGB = static_cast<AVFrame*>(vs->avFrameRGB);
                auto* sws  = static_cast<SwsContext*>(vs->swsCtx);

                AVPacket* pkt = av_packet_alloc();
                bool gotFrame = false;
                int decodeBudget = 30;  // bounded catch-up per update

                while (!gotFrame && decodeBudget-- > 0) {
                    int ret = av_read_frame(fmt, pkt);
                    if (ret == AVERROR_EOF) {
                        av_packet_unref(pkt);
                        if (vs->loop) {
                            // Rewind and keep decoding (loop mode); reset the
                            // pacing playhead so it does not fast-forward.
                            vs->playhead = 0.0;
                            const int sret = av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
                            if (sret < 0) { av_packet_unref(pkt); break; }
                            avcodec_flush_buffers(cc);
                            if (auto* aca = static_cast<AVCodecContext*>(vs->avAudioCodec)) {
                                avcodec_flush_buffers(aca);
                            }
                            continue;
                        }
                        ret = avcodec_send_packet(cc, nullptr);
                        if (ret >= 0 && avcodec_receive_frame(cc, f) == 0)
                            gotFrame = true;
                        // Drain any remaining buffered audio frames so the
                        // tail of the track is not truncated.
                        if (vs->audioEnabled && vs->swrCtx) {
                            auto* ac = static_cast<AVCodecContext*>(vs->avAudioCodec);
                            auto* af = static_cast<AVFrame*>(vs->avAudioFrame);
                            auto* swr = static_cast<SwrContext*>(vs->swrCtx);
                            if (ac && af && swr && avcodec_send_packet(ac, nullptr) >= 0) {
                                while (avcodec_receive_frame(ac, af) >= 0) {
                                    if (af->sample_rate <= 0 ||
                                        af->format != vs->expectedSampleFmt ||
                                        af->sample_rate != vs->expectedSampleRate ||
                                        !vs->expectedChLayout ||
                                        av_channel_layout_compare(&af->ch_layout,
                                            static_cast<const AVChannelLayout*>(vs->expectedChLayout.get())) != 0) {
                                        continue;
                                    }
                                    const int64_t outSamples =
                                        av_rescale_rnd(swr_get_delay(swr, af->sample_rate)
                                            + af->nb_samples, af->sample_rate,
                                            af->sample_rate, AV_ROUND_UP);
                                    std::vector<float> buf(static_cast<size_t>(outSamples) * 2);
                                    uint8_t* out[2] = {
                                        reinterpret_cast<uint8_t*>(buf.data()),
                                        reinterpret_cast<uint8_t*>(buf.data()) + outSamples * sizeof(float) };
                                    const int converted = swr_convert(swr, out, (int)outSamples,
                                        const_cast<const uint8_t**>(af->data), af->nb_samples);
                                    if (converted > 0) {
                                        std::lock_guard<std::mutex> lock(m_audioMutex);
                                        if (vs->audioQueue.size() < 8 * 1024 * 1024 / sizeof(float)) {
                                            vs->audioQueue.insert(vs->audioQueue.end(),
                                                buf.data(), buf.data() + converted * 2);
                                        }
                                    }
                                }
                            }
                        }
                        vs->ended = true;
                        vs->playing = false;
                        break;
                    }
                    if (ret < 0) { av_packet_unref(pkt); break; }

                    if (pkt->stream_index == vs->videoStreamIndex) {
                        ret = avcodec_send_packet(cc, pkt);
                        if (ret >= 0 && avcodec_receive_frame(cc, f) == 0)
                            gotFrame = true;
                    } else if (vs->audioEnabled && vs->swrCtx &&
                               pkt->stream_index == vs->audioStreamIndex) {
                        // Decode one audio packet and resample to interleaved
                        // float stereo into the shared audio queue.
                        auto* ac = static_cast<AVCodecContext*>(vs->avAudioCodec);
                        auto* af = static_cast<AVFrame*>(vs->avAudioFrame);
                        auto* swr = static_cast<SwrContext*>(vs->swrCtx);
                        if (ac && af && swr && avcodec_send_packet(ac, pkt) >= 0) {
                            while (avcodec_receive_frame(ac, af) >= 0) {
                                // Reject frames whose format/rate/layout differ
                                // from the swr configuration: converting them
                                // would read planes with the wrong sample size
                                // (heap OOB). Also guards sample_rate == 0,
                                // which would divide by zero in av_rescale_rnd.
                                if (af->sample_rate <= 0 ||
                                    af->format != vs->expectedSampleFmt ||
                                    af->sample_rate != vs->expectedSampleRate ||
                                    !vs->expectedChLayout ||
                                    av_channel_layout_compare(&af->ch_layout,
                                        static_cast<const AVChannelLayout*>(vs->expectedChLayout.get())) != 0) {
                                    continue;
                                }
                                const int64_t outSamples =
                                    av_rescale_rnd(swr_get_delay(swr, af->sample_rate)
                                        + af->nb_samples, af->sample_rate,
                                        af->sample_rate, AV_ROUND_UP);
                                std::vector<float> buf(static_cast<size_t>(outSamples) * 2);
                                uint8_t* out[2] = {
                                    reinterpret_cast<uint8_t*>(buf.data()),
                                    reinterpret_cast<uint8_t*>(buf.data()) + outSamples * sizeof(float) };
                                const int converted = swr_convert(swr, out, (int)outSamples,
                                    const_cast<const uint8_t**>(af->data), af->nb_samples);
                                if (converted > 0) {
                                    std::lock_guard<std::mutex> lock(m_audioMutex);
                                    if (vs->audioQueue.size() < 8 * 1024 * 1024 / sizeof(float)) {
                                        vs->audioQueue.insert(vs->audioQueue.end(),
                                            buf.data(), buf.data() + converted * 2);
                                    }
                                }
                            }
                        }
                    }
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);

                // Frame-rate pacing (mirrors the pl_mpeg path): decode up to
                // the budget, keeping the latest frame whose pts is not
                // beyond the playhead. A frame that is not yet due is never
                // dropped -- the loop simply continues until one is due.
                if (gotFrame && vs->playhead > 0.0) {
                    AVRational tb = fmt
                        ? fmt->streams[vs->videoStreamIndex]->time_base
                        : AVRational{1, 1};
                    if (f && f->pts != AV_NOPTS_VALUE) {
                        const double ptsSec = (double)f->pts * av_q2d(tb);
                        if (ptsSec > vs->playhead) {
                            // Not due yet: continue decoding (bounded by budget).
                            gotFrame = false;
                        }
                    }
                }

                if (gotFrame && cc && sws && fRGB) {
                    sws_scale(sws, f->data, f->linesize, 0, cc->height,
                              fRGB->data, fRGB->linesize);
                    frame->rgba.assign(vs->rgbaBuffer.begin(), vs->rgbaBuffer.end());
                    frame->valid = true;
                }
            },
            JobPriority::High
        );

        m_jobSystem->waitIdle();

        if (frame->valid) {
            const bgfx::Memory* mem = bgfx::copy(frame->rgba.data(), (uint32_t)frame->rgba.size());
            bgfx::updateTexture2D(vs->texture, 0, 0, 0, 0,
                                  (uint16_t)frame->width, (uint16_t)frame->height, mem);
            vs->hasFrame = true;
        }
        // Drain decoded audio regardless of whether a new video frame arrived
        // (mirrors the pl_mpeg path; without this the queued PCM is never
        // played).
        drainAudio(*vs);
        return frame->valid;
#else
        // FFmpeg not compiled →→ fall through to pl_mpeg below
#endif
    }

    // -------- pl_mpeg path (zero-dependency fallback) ----------------
    // Only reached when CAESURA_VIDEO_FFMPEG is OFF, or FFmpeg open() failed.
    {
        plm_t* plm = static_cast<plm_t*>(vs->plm);

        // Frame-rate pacing: advance the playhead by dt and decode enough
        // frames to catch up (bounded so a stall cannot block the frame).
        const double plmNow = plm_get_time(plm);
        // Loop rewound (plm_set_loop resets internal time to 0): re-sync the
        // playhead so the video does not fast-forward each loop. Hysteresis
        // ignores the sub-frame regressions the audio/video decoder
        // interleave can produce; real rewinds regress ~duration, so the
        // threshold is capped at duration/2 to never mask a short loop.
        const double loopHysteresis =
            std::min(0.25, (vs->duration > 0.0 ? vs->duration : 1.0) / 2.0);
        if (plmNow < vs->lastPlmTime - loopHysteresis) {
            vs->playhead = 0.0;
        }
        vs->lastPlmTime = plmNow;
        if (dt > 0.0 && vs->frameRate > 0.0) {
            vs->playhead += dt;
        }

        const int maxFrames = 30;
        auto frame = std::make_shared<DecodedFrame>();
        frame->width  = vs->width;
        frame->height = vs->height;

        m_jobSystem->submit(
            [this, frame, plm, vs]() {
                for (int n = 0; n < maxFrames; ++n) {
                    plm_frame_t* f = plm_decode_video(plm);
                    if (auto* samples = plm_decode_audio(plm)) {
                        onAudioDecoded(plm, samples);
                    }
                    if (f) {
                        frame->rgba.resize(frame->width * frame->height * 4);
                        plm_frame_to_rgba(f, frame->rgba.data(), frame->width * 4);
                        frame->valid = true;
                    }
                    // Stop once the decoded position reaches the playhead.
                    if (vs->frameRate <= 0.0 || plm_get_time(plm) >= vs->playhead - 0.5 / vs->frameRate)
                        break;
                    if (plm_has_ended(plm)) break;
                }
            },
            JobPriority::High
        );

        m_jobSystem->waitIdle();

        if (frame->valid) {
            const bgfx::Memory* mem = bgfx::copy(frame->rgba.data(), (uint32_t)frame->rgba.size());
            bgfx::updateTexture2D(vs->texture, 0, 0, 0, 0,
                                  (uint16_t)frame->width, (uint16_t)frame->height, mem);
            vs->hasFrame = true;
        }

        if (plm_has_ended(plm)) {
            vs->ended = true;
            vs->playing = false;
            DEBUG_INFO(SubSys::Render, ErrCode::Ok,
                       "VideoPlayer: video %u ended", handle.id);
        }

        drainAudio(*vs);
        return frame->valid;
    }

}

void VideoPlayer::updateAll(double dt) {
    if (m_videos.empty()) return;  // common case: no videos playing
    // Snapshot the ids: update() may stop/end videos (and close() may be
    // called from Lua mid-frame), so iterating the map directly is unsafe.
    std::vector<uint32_t> ids;
    ids.reserve(m_videos.size());
    for (const auto& kv : m_videos) ids.push_back(kv.first);
    for (const auto id : ids) {
        auto vs = find(VideoHandle{id});
        if (vs && vs->playing && !vs->ended) update(VideoHandle{id}, dt);
    }
    // Destroy videos closed this frame (RD-1: workers are idle here).
    flushPendingClose();
}

uint32_t VideoPlayer::getTexture(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    if (it == m_videos.end() || !it->second->hasFrame)
        return 0;
    return it->second->texture.idx;
}

bool VideoPlayer::isPlaying(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    return it != m_videos.end() && it->second->playing && !it->second->ended;
}

bool VideoPlayer::hasEnded(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    return it == m_videos.end() || it->second->ended;
}

int VideoPlayer::width(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    return it != m_videos.end() ? it->second->width : 0;
}

int VideoPlayer::height(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    return it != m_videos.end() ? it->second->height : 0;
}

double VideoPlayer::duration(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    return it != m_videos.end() ? it->second->duration : 0.0;
}

double VideoPlayer::currentTime(VideoHandle handle) const {
    auto it = m_videos.find(handle.id);
    if (it == m_videos.end()) return 0.0;

    if (it->second->useFFmpeg) {
#ifdef CAESURA_VIDEO_FFMPEG
        auto* cc = static_cast<AVCodecContext*>(it->second->avCodec);
        auto* f  = static_cast<AVFrame*>(it->second->avFrame);
        if (cc && f && f->pts != AV_NOPTS_VALUE) {
            AVRational tb = it->second->avFormat
                ? static_cast<AVFormatContext*>(it->second->avFormat)
                      ->streams[it->second->videoStreamIndex]->time_base
                : AVRational{1, 1};
            return (double)f->pts * av_q2d(tb);
        }
        return 0.0;
#else
        return 0.0;
#endif
    }
    if (!it->second->plm) return 0.0;
    return plm_get_time(static_cast<plm_t*>(it->second->plm));
}

void VideoPlayer::pause(VideoHandle handle) {
    auto vs = find(handle);
    if (vs) vs->playing = false;
}

void VideoPlayer::resume(VideoHandle handle) {
    auto vs = find(handle);
    if (vs && !vs->ended) vs->playing = true;
}

void VideoPlayer::seek(VideoHandle handle, double time) {
    auto vs = find(handle);
    if (!vs) return;

    // Stop and flush audio so post-seek PCM does not mix with pre-seek audio.
    auto* audio = BackendRegistry::instance().getAudioBackend();
    if (audio) {
        for (const auto h : vs->audioHandles) audio->stopSEHandle(h);
    }
    vs->audioHandles.clear();
    vs->audioHandle = 0;
    vs->audioStarted = false;
    { std::lock_guard<std::mutex> lk(m_audioMutex); vs->audioQueue.clear(); }

    time = clampSeekTime(time, vs->duration);

    if (vs->useFFmpeg) {
#ifdef CAESURA_VIDEO_FFMPEG
        auto* fmt = static_cast<AVFormatContext*>(vs->avFormat);
        auto* cc  = static_cast<AVCodecContext*>(vs->avCodec);
        int64_t ts = (int64_t)(time * (double)AV_TIME_BASE);
        int ret = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        if (ret >= 0 && cc) {
            avcodec_flush_buffers(cc);
            if (auto* aca = static_cast<AVCodecContext*>(vs->avAudioCodec)) {
                avcodec_flush_buffers(aca);
            }
        }
        vs->playhead = time;  // re-sync pacing (mirrors the plm branch)
#endif
    } else {
        if (vs->plm) {
            plm_seek(static_cast<plm_t*>(vs->plm), time, 0);
            // Re-sync the pacing playhead with the decoded position so the
            // frame-rate accumulator does not over-decode after a seek, and
            // update lastPlmTime so the loop-regression check in update()
            // cannot mistake this seek for a loop rewind. Read back the
            // actually landed keyframe time (plm_seek stops at the last
            // intra frame <= time, typically a GOP behind the request).
            const double landed = plm_get_time(static_cast<plm_t*>(vs->plm));
            vs->playhead = landed;
            vs->lastPlmTime = landed;
        }
    }
    vs->hasFrame = false;
}

void VideoPlayer::flushPendingClose() {
    // Called from updateAll()/shutdown() on the main thread, when no
    // decoder worker can still be touching a state (RD-1): every close()
    // deferred while a worker might be mid-frame is now destroyed.
    // Drop duplicates defensively: close() dedups, but a caller could
    // still push the same id twice across frames.
    std::vector<uint32_t> toClose;  // unique ids
    for (const auto id : m_pendingClose) {
        if (std::find(toClose.begin(), toClose.end(), id) == toClose.end())
            toClose.push_back(id);
    }
    m_pendingClose.clear();
    for (const auto id : toClose) {
        auto it = m_videos.find(id);
        if (it == m_videos.end()) continue;
        releaseState(*it->second);
        m_videos.erase(it);
    }
}

void VideoPlayer::shutdown() {
    // Flush any close()s deferred while a worker was mid-frame (RD-1);
    // no worker can be running during shutdown, so this is safe to do first.
    flushPendingClose();
    for (auto& [id, vs] : m_videos) {
        releaseState(*vs);
    }
    m_videos.clear();
}

std::shared_ptr<VideoPlayer::VideoState> VideoPlayer::find(VideoHandle handle) {
    auto it = m_videos.find(handle.id);
    return it != m_videos.end() ? it->second : nullptr;
}

void VideoPlayer::releaseState(VideoState& vs) {
    destroyTexture(vs);
#ifdef CAESURA_VIDEO_FFMPEG
    auto* sws = static_cast<SwsContext*>(vs.swsCtx);
    auto* frame = static_cast<AVFrame*>(vs.avFrame);
    auto* rgb = static_cast<AVFrame*>(vs.avFrameRGB);
    auto* codec = static_cast<AVCodecContext*>(vs.avCodec);
    auto* format = static_cast<AVFormatContext*>(vs.avFormat);
    if (sws) sws_freeContext(sws);
    av_frame_free(&frame); av_frame_free(&rgb); avcodec_free_context(&codec);
    if (format) avformat_close_input(&format);
    vs.swsCtx=nullptr;vs.avFrame=nullptr;vs.avFrameRGB=nullptr;vs.avCodec=nullptr;vs.avFormat=nullptr;
#endif
    if(vs.plm) { plm_destroy(static_cast<plm_t*>(vs.plm));vs.plm=nullptr; }
    vs.customIo.reset(); vs.memory.reset();
}

void VideoPlayer::destroyTexture(VideoState& vs) {
    if (bgfx::isValid(vs.texture)) {
        bgfx::destroy(vs.texture);
        vs.texture = BGFX_INVALID_HANDLE;
    }
#ifdef CAESURA_VIDEO_FFMPEG
    // Release FFmpeg audio resources (decoder context, frame, resampler).
    // The stored fields are void*; cast through typed locals for the free APIs.
    if (auto* swr = static_cast<SwrContext*>(vs.swrCtx)) {
        swr_free(&swr);
        vs.swrCtx = nullptr;
    }
    if (auto* af = static_cast<AVFrame*>(vs.avAudioFrame)) {
        av_frame_free(&af);
        vs.avAudioFrame = nullptr;
    }
    if (auto* ac = static_cast<AVCodecContext*>(vs.avAudioCodec)) {
        avcodec_free_context(&ac);
        vs.avAudioCodec = nullptr;
    }
    vs.expectedChLayout.reset();
    vs.audioStreamIndex = -1;
    vs.audioEnabled = false;
#endif
}

} // namespace Caesura

#include "doctest.h"
#include "../probes/SoakVoiceAdmission.h"
#include "audio/SoLoudAudioEngine.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "TestPaths.h"
#include <soloud_wav.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace Caesura;

namespace {
// Only the host quota is substituted. Decode, VOICE ownership, actual PCM mix,
// natural completion and the admission helper all use production code.
class SoakAudioQuota final : public ISandboxQuota {
public:
    SoakAudioQuota() : previous_(BackendRegistry::instance().getSandboxQuota()) {
        BackendRegistry::instance().setSandboxQuota(this);
    }
    ~SoakAudioQuota() override { BackendRegistry::instance().setSandboxQuota(previous_); }
    void setLuaState(lua_State*) override {}
    bool tryAlloc(const char*) override { ++live; return true; }
    void release(const char*) override { --live; }
    int count(const char*) override { return live; }
    int maxLimit(const char*) override { return 100; }
    int live = 0;
private:
    ISandboxQuota* previous_;
};

class ShortVoiceFile {
public:
    const std::string path = TestPaths::uniqueTempDir("soak_voice_admission").filename().string()+".wav";
    ShortVoiceFile() {
        constexpr unsigned rate=48000, frames=rate*80/1000;
        std::vector<uint8_t> bytes(44+frames*4);
        const auto word=[&](size_t at,uint32_t value,size_t count) {
            for(size_t i=0;i<count;++i)bytes[at+i]=uint8_t(value>>(i*8));
        };
        std::copy_n("RIFF",4,bytes.begin());std::copy_n("WAVEfmt ",8,bytes.begin()+8);
        std::copy_n("data",4,bytes.begin()+36);
        word(4,uint32_t(bytes.size()-8),4);word(16,16,4);word(20,1,2);word(22,2,2);
        word(24,rate,4);word(28,rate*4,4);word(32,4,2);word(34,16,2);word(40,frames*4,4);
        for(unsigned i=0;i<frames;++i) {
            word(44+i*4,uint16_t((int(i%97)-48)*120),2);
            word(46+i*4,uint16_t((int(i%71)-35)*140),2);
        }
        std::ofstream file(path,std::ios::binary);
        REQUIRE(file.good());
        file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        file.close();REQUIRE(file.good());
        // Verify the actual file with the same production WAV decoder. The
        // backend's getLength("voice") currently reports stream time instead.
        SoLoud::Wav decoded;
        REQUIRE(decoded.load(path.c_str())==SoLoud::SO_NO_ERROR);
        REQUIRE(decoded.getLength()==doctest::Approx(0.08).epsilon(0.0001));
    }
    ~ShortVoiceFile() { std::error_code ignored;std::filesystem::remove(path,ignored); }
};

std::array<unsigned,3> admitInterruptedSequence(SoLoudAudioEngine& audio,const std::string& path) {
    std::array<unsigned,3> handles{};
    handles[0]=audio.playVoice(path);audio.stopVoice();
    handles[1]=audio.playVoice(path);audio.stopVoice();
    handles[2]=audio.playVoice(path);
    REQUIRE(handles[0]!=0);REQUIRE(handles[1]!=0);REQUIRE(handles[2]!=0);
    REQUIRE(handles[0]!=handles[1]);REQUIRE(handles[0]!=handles[2]);REQUIRE(handles[1]!=handles[2]);
    return handles;
}

void mixThroughRealEof(SoLoudAudioEngine& audio) {
    REQUIRE(audio.soloud().getBackendId()==SoLoud::Soloud::NULLDRIVER);
    REQUIRE(audio.soloud().getBackendSamplerate()==48000);
    std::vector<float> pcm(8192*2);
    audio.soloud().mix(pcm.data(),8192); // 170.7 ms of mixer frames; no wall-clock sleeps.
    CHECK(std::all_of(pcm.begin(),pcm.end(),[](float v){return std::isfinite(v);}));
    CHECK(std::any_of(pcm.begin(),pcm.end(),[](float v){return std::abs(v)>1e-7f;}));
}
}

TEST_CASE("U27 voice admission: actual short VOICE remains admitted after natural EOF") {
    SoakAudioQuota quota;ShortVoiceFile file;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    CHECK(audio.getSnapshot().voiceCompletionsPending==0);
    const auto handles=admitInterruptedSequence(audio,file.path);
    const auto before=TestSupport::observeSoakVoiceAdmission(audio,41,handles);
    CHECK(before.supported);CHECK(before.playing);CHECK(before.completionsPending==0);
    CHECK(before.accepted());
    // Merely observing cannot advance the manual mixer or consume a completion.
    CHECK(TestSupport::observeSoakVoiceAdmission(audio,41,handles).playing);
    CHECK(audio.getSnapshot().voiceCompletionsPending==0);
    mixThroughRealEof(audio);
    const auto after=TestSupport::observeSoakVoiceAdmission(audio,41,handles);
    CHECK_FALSE(after.playing);CHECK(after.completionsPending==1);
    CHECK(after.accepted()); // Same test is RED with the preserved playing-only predicate.
    const auto repeated=TestSupport::observeSoakVoiceAdmission(audio,41,handles);
    CHECK_FALSE(repeated.playing);CHECK(repeated.completionsPending==1);CHECK(repeated.accepted());
    CHECK(audio.getSnapshot().voiceCompletionsPending==1);
    CHECK(audio.consumeVoiceCompletions()==1);CHECK(audio.consumeVoiceCompletions()==0);
    audio.shutdown();CHECK(quota.live==0);
}

TEST_CASE("U27 voice admission: explicit stop and missing source do not imitate natural completion") {
    SoakAudioQuota quota;ShortVoiceFile file;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    auto handles=admitInterruptedSequence(audio,file.path);
    REQUIRE(TestSupport::observeSoakVoiceAdmission(audio,42,handles).accepted());
    audio.stopVoice();
    std::vector<float> pcm(8192*2);audio.soloud().mix(pcm.data(),8192);
    const auto stopped=TestSupport::observeSoakVoiceAdmission(audio,42,handles);
    CHECK_FALSE(stopped.playing);CHECK(stopped.completionsPending==0);CHECK_FALSE(stopped.accepted());
    CHECK(audio.consumeVoiceCompletions()==0);
    handles[2]=audio.playVoice(file.path+".missing");
    REQUIRE(handles[2]==0);
    CHECK_FALSE(TestSupport::observeSoakVoiceAdmission(audio,42,handles).accepted());
    CHECK(audio.getSnapshot().voiceCompletionsPending==0);
    audio.shutdown();CHECK(quota.live==0);
}

TEST_CASE("U27 voice admission: actual stale and duplicate completions are rejected without consumption") {
    SoakAudioQuota quota;ShortVoiceFile file;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    auto handles=admitInterruptedSequence(audio,file.path);
    mixThroughRealEof(audio);
    REQUIRE(TestSupport::observeSoakVoiceAdmission(audio,43,handles).completionsPending==1);
    // Keep the first real EOF queued. A newly playing source plus this old
    // completion is the forbidden true/1 state, not a successful admission.
    handles=admitInterruptedSequence(audio,file.path);
    const auto stale=TestSupport::observeSoakVoiceAdmission(audio,44,handles);
    CHECK(stale.playing);CHECK(stale.completionsPending==1);CHECK_FALSE(stale.accepted());
    CHECK(audio.getSnapshot().voiceCompletionsPending==1);
    mixThroughRealEof(audio);
    const auto duplicated=TestSupport::observeSoakVoiceAdmission(audio,44,handles);
    CHECK_FALSE(duplicated.playing);CHECK(duplicated.completionsPending==2);CHECK_FALSE(duplicated.accepted());
    CHECK(audio.getSnapshot().voiceCompletionsPending==2);
    CHECK(audio.consumeVoiceCompletions()==2);CHECK(audio.consumeVoiceCompletions()==0);
    audio.shutdown();CHECK(quota.live==0);
}

TEST_CASE("U27 voice admission: real playback still requires positive async identity and all distinct handles") {
    SoakAudioQuota quota;ShortVoiceFile file;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    const auto handles=admitInterruptedSequence(audio,file.path);
    CHECK_FALSE(TestSupport::observeSoakVoiceAdmission(audio,0,handles).accepted());
    CHECK_FALSE(TestSupport::observeSoakVoiceAdmission(audio,-1,handles).accepted());
    for(const auto duplicate:std::array<std::array<unsigned,3>,3>{{
        {{handles[0],handles[0],handles[2]}},{{handles[0],handles[1],handles[0]}},{{handles[0],handles[1],handles[1]}}
    }})CHECK_FALSE(TestSupport::observeSoakVoiceAdmission(audio,45,duplicate).accepted());
    CHECK(TestSupport::observeSoakVoiceAdmission(audio,45,handles).accepted());
    CHECK(audio.getSnapshot().voiceCompletionsPending==0);
    audio.stopVoice();audio.shutdown();CHECK(quota.live==0);
}

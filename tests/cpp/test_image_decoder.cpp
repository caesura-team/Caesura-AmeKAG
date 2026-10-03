#include "doctest.h"
#include "resource/ImageDecoder.h"
#include "resource/AssetManager.h"
#include <cstdint>
#include <memory>

using namespace Caesura;

namespace {

class LifetimeTrackingProvider final : public Caesura::IAssetProvider {
public:
    Caesura::AssetDirectoryResult listDirectory(const std::string&, size_t, size_t) override { return {}; }
    explicit LifetimeTrackingProvider(int& destructionCount)
        : m_destructionCount(destructionCount) {}

    ~LifetimeTrackingProvider() override { ++m_destructionCount; }

    std::vector<uint8_t> read(const std::string&) override { return {}; }
    bool exists(const std::string&) override { return false; }
    std::string getSource() const override { return "lifetime-test"; }
    int priority() const override { return 0; }
    bool verify() override { return true; }

private:
    int& m_destructionCount;
};

} // namespace

TEST_CASE("U11 CPU decoder: real pixels prepare without GPU and obey the output bound") {
    // An uncompressed, one-pixel, 24-bit BMP. This avoids depending on the
    // malformed embedded PNG that the previous placeholder mentioned.
    std::vector<uint8_t> bmp(58,0);
    bmp[0]='B'; bmp[1]='M'; bmp[2]=58; bmp[10]=54; bmp[14]=40;
    bmp[18]=1; bmp[22]=1; bmp[26]=1; bmp[28]=24; bmp[34]=4;
    bmp[54]=3; bmp[55]=2; bmp[56]=1;
    CpuImageDecoder implementation;
    IImageDecoder& decoder=implementation;
    auto pixels=decoder.decode(bmp.data(),bmp.size(),4);
    REQUIRE(pixels.ok);
    CHECK(pixels.width==1);
    CHECK(pixels.height==1);
    CHECK(pixels.rgba==std::vector<uint8_t>{1,2,3,255});
    bmp[54]=99;
    CHECK(pixels.rgba[2]==3);
    CHECK_FALSE(decoder.decode(bmp.data(),bmp.size(),3).ok);
    CHECK_FALSE(decoder.decode(nullptr,0,4).ok);
    for (size_t length=0; length<57; ++length) {
        CAPTURE(length);
        CHECK_FALSE(decoder.decode(bmp.data(),length,4).ok);
    }
}

TEST_CASE("U11 CPU decoder: complete TGA and KTX payloads are required") {
    CpuImageDecoder decoder;
    std::vector<uint8_t> tga(21, 0);
    tga[2] = 2; tga[12] = 1; tga[14] = 1; tga[16] = 24;
    tga[18] = 3; tga[19] = 2; tga[20] = 1;
    const auto tgaImage = decoder.decode(tga.data(), tga.size(), 4);
    REQUIRE(tgaImage.ok);
    CHECK(tgaImage.rgba == std::vector<uint8_t>{1,2,3,255});
    for (size_t length = 0; length < tga.size(); ++length) {
        CAPTURE(length);
        CHECK_FALSE(decoder.decode(tga.data(), length, 4).ok);
    }
    std::vector<uint8_t> ktx = {
        0xab,'K','T','X',' ', '1','1',0xbb,0x0d,0x0a,0x1a,0x0a,
        1,2,3,4, 1,0x14,0,0, 1,0,0,0, 8,0x19,0,0,
        0x58,0x80,0,0, 8,0x19,0,0, 1,0,0,0, 1,0,0,0,
        0,0,0,0, 0,0,0,0, 1,0,0,0, 1,0,0,0,
        0,0,0,0, 4,0,0,0, 1,2,3,255
    };
    const auto ktxImage = decoder.decode(ktx.data(), ktx.size(), 4);
    REQUIRE(ktxImage.ok);
    CHECK(ktxImage.rgba == std::vector<uint8_t>{1,2,3,255});
    for (size_t length = 0; length < ktx.size(); ++length) {
        CAPTURE(length);
        CHECK_FALSE(decoder.decode(ktx.data(), length, 4).ok);
    }
}

TEST_CASE("AssetManager local instance lifecycle") {
    AssetManager am;

    int destructionCount = 0;
    for (int cycle = 1; cycle <= 2; ++cycle) {
        am.init();
        const bool scriptFound = am.exists("scripts/config.lua") || am.exists("config.lua");
        CHECK(scriptFound);

        am.addProvider(std::make_unique<LifetimeTrackingProvider>(destructionCount));
        am.shutdown();
        CHECK(destructionCount == cycle);
        CHECK_FALSE(am.exists("scripts/config.lua"));
    }

    CHECK_NOTHROW(am.shutdown());

    int destructorCount = 0;
    {
        AssetManager scoped;
        scoped.init();
        scoped.addProvider(std::make_unique<LifetimeTrackingProvider>(destructorCount));
    }
    CHECK(destructorCount == 1);
}

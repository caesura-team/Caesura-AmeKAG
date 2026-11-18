// carc_pack.exe — CARC archive tool
// Usage:
//   carc_pack.exe <input_dir> <output.carc> [public.key] [private.key]
//   carc_pack.exe list <archive.carc> [public.key]
//   carc_pack.exe extract <archive.carc> <out_dir> [public.key]
//
// If key paths are omitted, keys are embedded in the archive only.
// Key files are raw binary (32 bytes public, 64 bytes private).
// `list` prints archive file names one per line (machine-readable, for
// the KAG3 importer's --carc mode); `extract` restores all files.

#include "archive/ArchivePath.h"
#include "archive/CARCReader.h"
#include "archive/CARCWriter.h"
#include "archive/DeltaCARC.h"
#include "ExtractionOutput.h"
#include <charconv>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;
using Caesura::carc::pathFromUtf8;

static void printUsage() {
    std::cerr << "Usage: carc_pack.exe <input_dir> <output.carc> [public.key] [private.key]\n"
              << "       carc_pack.exe pack <input_dir> <output.carc> [public.key] [private.key]\n"
              << "       carc_pack.exe list <archive.carc> [public.key]\n"
              << "       carc_pack.exe extract <archive.carc> <out_dir> [public.key] [--path rel]\n"
              << "       carc_pack.exe delta <old.carc> <new.carc> <delta.carc>\n"
              << "       carc_pack.exe apply <base.carc> <delta.carc> <output.carc>\n"
              << "       carc_pack.exe verify-delta <delta.carc>\n"
              << "  input_dir      directory to pack\n"
              << "  output.carc    CARC archive to create\n"
              << "  public.key     (optional) path to public key (verify on read)\n"
              << "  private.key    (optional) path to save private key\n"
              << "  list           print the archive's file names (one per line)\n"
              << "  extract        extract all files into out_dir\n"
              << "  delta/patch    generate differential patch between two CARC archives\n"
              << "  apply          apply differential patch to base archive to produce output archive\n"
              << "  verify-delta   verify cryptographic integrity and structure of delta patch\n";
}

static std::string relativePath(const fs::path& filePath, const fs::path& baseDir)
{
    return fs::relative(filePath, baseDir).generic_string();
}

static int doDelta(int argc, char* argv[])
{
    if (argc < 5) {
        std::cerr << "Usage: carc_pack.exe delta <old.carc> <new.carc> <delta.carc>\n";
        return 1;
    }
    std::string oldPath = argv[2];
    std::string newPath = argv[3];
    std::string deltaPath = argv[4];

    if (!fs::exists(pathFromUtf8(oldPath))) {
        std::cerr << "Error: old archive not found: " << oldPath << "\n";
        return 1;
    }
    if (!fs::exists(pathFromUtf8(newPath))) {
        std::cerr << "Error: new archive not found: " << newPath << "\n";
        return 1;
    }

    if (!Caesura::carc::DeltaCARC::generate(oldPath, newPath, deltaPath)) {
        std::cerr << "Error: failed to generate delta patch from " << oldPath
                  << " to " << newPath << "\n";
        return 1;
    }
    std::cout << "Created differential patch: " << deltaPath << "\n";
    return 0;
}

static int doApply(int argc, char* argv[])
{
    if (argc < 5) {
        std::cerr << "Usage: carc_pack.exe apply <base.carc> <delta.carc> <output.carc>\n";
        return 1;
    }
    std::string basePath = argv[2];
    std::string deltaPath = argv[3];
    std::string outputPath = argv[4];

    if (!fs::exists(pathFromUtf8(basePath))) {
        std::cerr << "Error: base archive not found: " << basePath << "\n";
        return 1;
    }
    if (!fs::exists(pathFromUtf8(deltaPath))) {
        std::cerr << "Error: delta patch not found: " << deltaPath << "\n";
        return 1;
    }

    if (!Caesura::carc::DeltaCARC::apply(basePath, deltaPath, outputPath)) {
        std::cerr << "Error: failed to apply delta patch " << deltaPath
                  << " to base " << basePath << "\n";
        return 1;
    }
    std::cout << "Applied delta patch: " << outputPath << "\n";
    return 0;
}

static int doVerifyDelta(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: carc_pack.exe verify-delta <delta.carc>\n";
        return 1;
    }
    std::string deltaPath = argv[2];
    if (!fs::exists(pathFromUtf8(deltaPath))) {
        std::cerr << "Error: delta patch file not found: " << deltaPath << "\n";
        return 1;
    }

    if (!Caesura::carc::DeltaCARC::verify(deltaPath)) {
        std::cerr << "Error: delta patch verification failed: " << deltaPath << "\n";
        return 1;
    }
    std::cout << "Delta patch verified successfully: " << deltaPath << "\n";
    return 0;
}

static int doList(int argc, char* argv[])
{
    if (argc < 3) {
        printUsage();
        return 1;
    }
    std::string carcPath = argv[2];
    std::string pubKey = (argc > 3) ? argv[3] : "";
    Caesura::carc::CARCReader reader;
    if (!reader.open(carcPath, pubKey)) {
        std::cerr << "Error: cannot open archive (bad key or corrupt file): "
                  << carcPath << "\n";
        return 1;
    }
    for (const auto& name : reader.fileList()) {
        std::cout << name << "\n";
    }
    return 0;
}

static std::vector<uint8_t> readNamedMember(Caesura::carc::CARCReader& reader,
                                           const fs::path& path, const std::string& label)
{
    // Pack still uses native narrow spelling for member hashes.
    const auto name = path.string();
    if (!reader.hasFile(name)) {
        std::cerr << "Error: not in archive: " << label << "\n";
        return {};
    }
    auto data = reader.readFile(name);
    if (data.empty()) std::cerr << "Error: empty read for " << label << "\n";
    return data;
}

static std::string decodePathArgument(std::string_view hex)
{
    if (hex.size() % 2 != 0) return {};
    std::string path;
    path.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned byte = 0;
        const auto end = hex.data() + i + 2;
        const auto parsed = std::from_chars(end - 2, end, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end || byte == 0) return {};
        path.push_back(static_cast<char>(byte));
    }
    return path;
}

// Internal pipe protocol for the Lua importer. Only ASCII hex crosses its
// shell boundary, and stdout contains the verified member bytes alone.
static int doReadMember(int argc, char* argv[]) try
{
    if (argc != 4) {
        std::cerr << "Error: expected a hex-encoded archive and member path\n";
        return 1;
    }
    const auto archive = decodePathArgument(argv[2]);
    const auto member = decodePathArgument(argv[3]);
    const auto path = pathFromUtf8(member);
    if (archive.empty() || !Caesura::carc_tool::validMemberPath(path)) {
        std::cerr << "Error: invalid archive or member path\n";
        return 1;
    }
    Caesura::carc::CARCReader reader;
    if (!reader.open(archive)) {
        std::cerr << "Error: cannot open archive: " << archive << "\n";
        return 1;
    }
    const auto data = readNamedMember(reader, path, member);
    if (data.empty()) return 1;
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::cerr << "Error: cannot set archive member output mode\n";
        return 1;
    }
#endif
    std::cout.write(reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size())).flush();
    return std::cout ? 0 : 1;
} catch (const std::system_error& error) {
    std::cerr << "Error: CARC member read failed: " << error.what() << "\n";
    return 1;
}

static int doExtract(int argc, char* argv[]) try
{
    // carc_pack.exe extract <archive.carc> <out_dir> [public.key] [--path rel]
    if (argc < 4) {
        printUsage();
        return 1;
    }
    std::string carcPath = argv[2];
    std::string outDir = argv[3];
    std::string pubKey = "";
    std::string onlyPath = "";
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--path") {
            if (!onlyPath.empty() || i + 1 == argc || argv[i + 1][0] == '\0') {
                std::cerr << "Error: --path requires exactly one nonempty member path\n";
                return 1;
            }
            onlyPath = argv[++i];
        } else if (pubKey.empty()) {
            pubKey = a;
        } else {
            std::cerr << "Error: unexpected extract argument: " << a << "\n";
            return 1;
        }
    }
    namespace output = Caesura::carc_tool;
    const auto memberPath = pathFromUtf8(onlyPath);
    if (!onlyPath.empty() && !output::validMemberPath(memberPath)) {
        std::cerr << "Error: invalid archive member path: " << onlyPath << "\n";
        return 1;
    }

    Caesura::carc::CARCReader reader;
    if (!reader.open(carcPath, pubKey)) {
        std::cerr << "Error: cannot open archive (bad key or corrupt file): "
                  << carcPath << "\n";
        return 1;
    }
    auto outputRoot = pathFromUtf8(outDir);
    // With --path: extract exactly one known relative path (the CARC
    // stores only path hashes, so the caller must know the name).
    if (!onlyPath.empty()) {
        const auto data = readNamedMember(reader, memberPath, onlyPath);
        if (data.empty()) return 1;
        if (!output::prepareOutputRoot(outputRoot) ||
            !output::writeExtractedFile(outputRoot, memberPath, data)) {
            std::cerr << "Error: cannot write safe extraction path: " << onlyPath << "\n";
            return 1;
        }
        std::cout << "Extracted: " << onlyPath << "\n";
        return 0;
    }
    // Full extract: dump everything by its path hash name (the original
    // names are not stored; callers with --path get real names back).
    if (!output::prepareOutputRoot(outputRoot)) {
        std::cerr << "Error: cannot use extraction directory: " << outDir << "\n";
        return 1;
    }
    size_t count = 0;
    for (const auto& name : reader.fileList()) {
        // hex hash -> bytes -> readFileByHash
        uint8_t hash[Caesura::carc::PATH_HASH_SIZE] = {0};
        bool okHex = name.size() == Caesura::carc::PATH_HASH_SIZE * 2;
        if (okHex) {
            for (size_t i = 0; i < Caesura::carc::PATH_HASH_SIZE; i++) {
                auto hexToNib = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hexToNib(name[i * 2]);
                int lo = hexToNib(name[i * 2 + 1]);
                if (hi < 0 || lo < 0) { okHex = false; break; }
                hash[i] = static_cast<uint8_t>((hi << 4) | lo);
            }
        }
        if (!okHex) {
            std::cerr << "Warning: skip malformed hash " << name << "\n";
            continue;
        }
        auto data = reader.readFileByHash(hash);
        if (data.empty()) {
            std::cerr << "Warning: empty read for " << name << "\n";
            continue;
        }
        if (!output::writeExtractedFile(outputRoot, name, data)) {
            std::cerr << "Error: cannot write safe extraction path: " << name << "\n";
            return 1;
        }
        ++count;
    }
    std::cout << "Extracted: " << count << " files into " << outDir << "\n";
    return 0;
} catch (const std::system_error& error) {
    std::cerr << "Error: CARC extraction failed: " << error.what() << "\n";
    return 1;
}

static int run(int argc, char* argv[])
{
    // subcommand dispatch
    if (argc >= 2) {
        std::string cmd = argv[1];
        if (cmd == "list") {
            return doList(argc, argv);
        }
        if (cmd == "extract") {
            return doExtract(argc, argv);
        }
        if (cmd == "read-member-hex") {
            return doReadMember(argc, argv);
        }
        if (cmd == "delta" || cmd == "patch") {
            return doDelta(argc, argv);
        }
        if (cmd == "apply") {
            return doApply(argc, argv);
        }
        if (cmd == "verify-delta" || cmd == "verify") {
            return doVerifyDelta(argc, argv);
        }
    }

    // ---- pack mode (original or explicit 'pack' subcommand) ----
    int argOffset = 0;
    if (argc >= 2 && std::string(argv[1]) == "pack") {
        argOffset = 1;
    }

    if (argc < 3 + argOffset) {
        printUsage();
        return 1;
    }

    std::string inputDir = argv[1 + argOffset];
    std::string outputCarc = argv[2 + argOffset];
    std::string pubKeyPath = (argc > 3 + argOffset) ? argv[3 + argOffset] : "";
    std::string privKeyPath = (argc > 4 + argOffset) ? argv[4 + argOffset] : "";

    const auto sourceRoot = pathFromUtf8(inputDir);
    if (!fs::is_directory(sourceRoot)) {
        std::cerr << "Error: input directory not found: " << inputDir << "\n";
        return 1;
    }

    Caesura::carc::CARCWriter writer;
    if (!writer.create(outputCarc, privKeyPath, pubKeyPath)) {
        std::cerr << "Error: failed to create output file: " << outputCarc << "\n";
        return 1;
    }

    std::cout << "Packing directory: " << inputDir << "\n";
    size_t fileCount = 0;

    for (const auto& entry : fs::recursive_directory_iterator(sourceRoot)) {
        if (!entry.is_regular_file()) continue;

        fs::path filePath = entry.path();
        std::string relPath = relativePath(filePath, sourceRoot);

        // Read file
        std::ifstream in(filePath, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            std::cerr << "Warning: cannot read " << filePath << "\n";
            continue;
        }
        std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), size);

        if (!writer.addFile(relPath, data.data(), data.size())) {
            std::cerr << "Error: failed to add file: " << relPath << "\n";
            return 1;
        }
        ++fileCount;
    }

    if (!writer.finalize()) {
        std::cerr << "Error: finalize failed\n";
        return 1;
    }

    std::cout << "Created: " << outputCarc << " (" << fileCount << " files)\n";
    if (!pubKeyPath.empty())
        std::cout << "Public key: " << pubKeyPath << "\n";
    if (!privKeyPath.empty())
        std::cout << "Private key: " << privKeyPath << "\n";

    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8;
    utf8.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        utf8.push_back(Caesura::carc::pathToUtf8(fs::path(argv[i])));
    }
    std::vector<char*> args;
    args.reserve(utf8.size());
    for (auto& arg : utf8) args.push_back(arg.data());
    return run(argc, args.data());
}
#else
int main(int argc, char* argv[]) {
    return run(argc, argv);
}
#endif

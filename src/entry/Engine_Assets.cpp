// Engine_Assets.cpp -- composition-root registration of CARC asset providers.
//
// LAYERED VFS MOUNT POLICY (C1)
// -----------------------------------------------------------------------------
// The engine mounts every recognized .carc it finds next to the executable's
// working directory plus an optional dlc/ subdirectory, ordered by priority
// (ProviderChain::sortByPriority resolves reads highest-first):
//
//   40  patch.carc, patch_*.carc     hotfix layer -- overrides everything
//   30  dlc_*.carc, dlc/*.carc       downloadable content
//   20  lang_*.carc                  localization packs
//   10  base.carc, data.carc, game.carc   shipped game data
//   (5)  DirAssetProvider "" and "assets"  -- loose files, registered by
//        AssetManager::init(), always the lowest layer
//
// A file whose name matches none of those patterns is DELIBERATELY NOT mounted:
// an unrecognized .carc in the game directory stays inert instead of joining
// the chain at a guessed priority.
//
// HOST PUBLISHER TRUST (U8)
// -----------------------------------------------------------------------------
// Compatible mode verifies self-signatures; it does not identify a publisher.
// PinnedPublisher requires the host's fixed key for every recognized CARC layer
// before any archive provider is inserted. A rejected layer fails initialization
// rather than skipping to lower CARC or loose providers. This does not authenticate
// loose resources, entry Lua, manifests, or a replaceable host executable.

#include "EngineConfig.h"
#include "../archive/ArchivePath.h"
#include "../archive/CARCReader.h"
#include "../archive/CarcAssetProvider.h"
#include "../resource/AssetManager.h"
#include "../debug/api/DebugLog.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <set>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace Caesura {

// Mount priorities. Named so the table is greppable from the tests and from
// the docs instead of being seven magic numbers inline.
constexpr int kCarcPriorityPatch = 40;
constexpr int kCarcPriorityDlc   = 30;
constexpr int kCarcPriorityLang  = 20;
constexpr int kCarcPriorityBase  = 10;
constexpr int kCarcPriorityNone  = -1;  // unrecognized -> not mounted

// Classify one archive file name. `inDlcDir` marks files found inside dlc/,
// which are treated as DLC regardless of their file name.
// Exposed (not in an anonymous namespace) so tests can pin the table.
int carcMountPriority(const std::string& filename, bool inDlcDir) {
    if (filename == "patch.carc" || filename.rfind("patch_", 0) == 0) {
        return kCarcPriorityPatch;
    }
    if (filename.rfind("dlc_", 0) == 0 || inDlcDir) {
        return kCarcPriorityDlc;
    }
    if (filename.rfind("lang_", 0) == 0) {
        return kCarcPriorityLang;
    }
    if (filename == "base.carc" || filename == "data.carc" || filename == "game.carc") {
        return kCarcPriorityBase;
    }
    return kCarcPriorityNone;
}

// One resolved mount candidate. Internal to this translation unit: tests
// observe the plan through carcMountPlan() below, which returns a flat
// (name, priority) list so no aggregate type has to cross a TU boundary.
namespace {
struct CarcMountCandidate {
    std::string path;   // path to open
    std::string name;   // file name (identity for dedup + logging)
    std::string sortName;
    int         priority;
};
}  // namespace

// Collect the mounts for a game root: root/*.carc plus root/dlc/*.carc,
// sorted by descending priority then by name so the order is deterministic
// regardless of directory-iteration order.
//
// Dedup is keyed by FILE NAME, not by the iterator's path spelling. The
// earlier draft keyed on fs::path::generic_string() ("./base.carc" when
// scanning ".") and then re-checked a fallback list with the bare name
// ("base.carc"), so the key never matched and base.carc was mounted TWICE --
// observed as two "Registered CARC (priority 10)" lines for one file. Two
// providers over the same archive is not merely noisy: each holds its own
// open stream and read position for the same bytes.
static std::vector<CarcMountCandidate> collectCarcMounts(
    const fs::path& root, bool* scanSucceeded = nullptr) {
    std::vector<CarcMountCandidate> candidates;
    std::set<std::string> seenNames;
    bool scanOk = true;

    auto scan = [&](const fs::path& dir, bool inDlcDir) {
        std::error_code ec;
        const bool exists = fs::exists(dir, ec);
        if (ec || (!exists && !inDlcDir)) { scanOk = false; return; }
        if (!exists) return; // The optional dlc directory may be absent.
        if (!fs::is_directory(dir, ec) || ec) { scanOk = false; return; }
        fs::directory_iterator it(dir, ec), end;
        if (ec) { scanOk = false; return; }
        while (it != end) {
            const auto& entry = *it;
            const bool regular = entry.is_regular_file(ec);
            if (ec) { scanOk = false; return; }
            if (regular && entry.path().extension() == ".carc") {
                const auto leaf = entry.path().filename();
                const std::string filename = carc::pathToUtf8(leaf);
                const int priority = carcMountPriority(filename, inDlcDir);
                if (priority != kCarcPriorityNone && seenNames.insert(filename).second) {
                    std::string sortName = filename;
#ifdef _WIN32
                    try {
                        // Keep the old order for names the code page can spell.
                        sortName = leaf.string();
                    } catch (const std::system_error&) {
                        // Use UTF-8 for a name that was not mountable before.
                    }
#endif
                    candidates.push_back(
                        {carc::pathToUtf8(entry.path()), filename, std::move(sortName), priority});
                }
            }
            it.increment(ec);
            if (ec) { scanOk = false; return; }
        }
    };

    scan(root, false);
    scan(root / "dlc", true);
    if (scanSucceeded) *scanSucceeded = scanOk;

    std::sort(candidates.begin(), candidates.end(),
              [](const CarcMountCandidate& a, const CarcMountCandidate& b) {
                  if (a.priority != b.priority) return a.priority > b.priority;
                  if (a.sortName != b.sortName) return a.sortName < b.sortName;
                  return a.name < b.name;
              });
    return candidates;
}

// Test seam: the resolved mount plan for `root`, as (file name, priority) in
// mount order. Lets the mount policy -- classification, dedup, ordering, and
// the "unrecognized names stay inert" rule -- be asserted without a GPU, an
// Engine instance, or a process-wide chdir.
std::vector<std::pair<std::string, int>> carcMountPlan(const std::string& root) {
    std::vector<std::pair<std::string, int>> plan;
    for (const auto& cand : collectCarcMounts(carc::pathFromUtf8(root))) {
        plan.emplace_back(cand.name, cand.priority);
    }
    return plan;
}

bool registerDefaultAssetProviders(AssetManager& assetManager,
                                   const EngineConfig& config, const std::string& root) {
    const bool pinned = config.archiveTrustMode == ArchiveTrustMode::PinnedPublisher;
    if ((config.archiveTrustMode != ArchiveTrustMode::Compatible && !pinned) ||
        (pinned && !config.archivePublisherKey)) {
        fprintf(stderr, "[Engine] CARC trust configuration invalid: pinned mode requires a host public key.\n");
        return false;
    }

    printf("[Engine] CARC trust: %s. Loose resources, entry Lua, manifests and the host executable remain unauthenticated.\n",
           pinned ? "host-pinned publisher" : "compatible self-signature (publisher unauthenticated)");
    std::vector<std::unique_ptr<IAssetProvider>> staged;
    bool scanSucceeded = false;
    const auto candidates = collectCarcMounts(carc::pathFromUtf8(root), &scanSucceeded);
    if (pinned && !scanSucceeded) {
        fprintf(stderr, "[Engine] CARC mount discovery failed; strict initialization rejected.\n");
        return false;
    }
    for (const auto& candidate : candidates) {
        auto reader = std::make_unique<carc::CARCReader>();
        const bool opened = pinned
            ? reader->open(candidate.path, *config.archivePublisherKey)
            : reader->open(candidate.path);
        if (!opened) {
            fprintf(stderr, "[Engine] CARC verification failed (%s): %s\n",
                    pinned ? "host-pinned publisher; initialization rejected" : "compatible; skipped",
                    candidate.path.c_str());
            if (pinned) return false;
            continue;
        }
        staged.push_back(std::make_unique<carc::CarcAssetProvider>(
            std::move(reader), candidate.priority, "CARC:" + candidate.name));
    }

    // All strict candidates have passed before the first provider is visible.
    for (auto& provider : staged) {
        printf("[Engine] Registered CARC (priority %d, %s): %s\n",
               provider->priority(), pinned ? "publisher authenticated" : "self-signature only",
               provider->getSource().c_str());
        if (provider->priority() == kCarcPriorityPatch) {
            DEBUG_WARN(SubSys::Archive, ErrCode::Ok,
                       "[Engine] Patch layer active: %s overrides shipped content",
                       provider->getSource().c_str());
        }
        assetManager.addProvider(std::move(provider));
    }
    return true;
}

} // namespace Caesura

// ProjectService.cpp -- implementation extracted from EditorServer.cpp
// (task book §14 layering; behavior parity is enforced by headless_http_smoke).

#include "ProjectService.h"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace Caesura {
namespace rpc {
namespace service {

namespace {

std::string nowIsoUtc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Json defaultProjectMeta(const std::string& name) {
    const std::string now = nowIsoUtc();
    return Json{
        {"name", name},
        {"template", ""},
        {"version", "1.0"},
        {"language", "zh"},
        {"description", ""},
        {"created", now},
        {"modified", now},
    };
}

void overlayStringMeta(Json& base, const Json& stored) {
    for (auto it = stored.begin(); it != stored.end(); ++it) {
        if (it.value().is_string()) base[it.key()] = it.value();
    }
}

// --- Import-source confinement (Sprint 1 t4) ---------------------------
// /api/project/import used to hand srcPath straight to fs::copy, which made it
// an arbitrary-directory read primitive: any local caller could name any
// directory that happens to contain story.ks/entry.lua and have it copied into
// the projects root (where /api/project/meta and the editor then read it).
//
// Policy: the source must resolve, after canonicalization, INSIDE one of the
// allowed import roots -- the projects root, the engine source root, or a root
// the operator opted into via CAESURA_EDITOR_IMPORT_ROOTS (platform path-list
// separator). Everything else is refused with a distinct status so the editor
// can tell "outside allowed roots" from "does not exist".
//
// The normalization rules mirror src/live2d/PathConfinement.cpp (UNC rejection,
// drive-letter handling, backslash folding, case-insensitive compare on
// Windows only). That logic is NOT included across module boundaries
// (AGENTS.md §1: no cross-module include of concrete implementations), so the
// equivalent is implemented here inside the rpc module.

constexpr char kImportRootsEnv[] = "CAESURA_EDITOR_IMPORT_ROOTS";

// A path string reaches this service in one of two encodings: UTF-8 (the JSON
// HTTP body) or the platform-native narrow encoding (an in-process C++ caller
// passing fs::path::string()). MSVC's std::filesystem::path decodes a NARROW
// string with the active code page, so feeding it UTF-8 mangles or throws when
// the path contains non-ASCII bytes -- a repository under a CJK directory made
// /api/project/import answer an empty HTTP 500. Decode as UTF-8 first (via
// char8_t, the same explicit route ProjectContext takes for its u8""
// CAESURA_SOURCE_DIR macro) and fall back to the native decoding only when the
// UTF-8 reading does not name an existing directory.
fs::path pathFromUtf8(const std::string& text) {
    try {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                                      text.size()));
    } catch (const std::exception&) {
        return {};
    }
}

fs::path pathFromRequest(const std::string& text) {
    const auto resolves = [](const fs::path& candidate) {
        if (candidate.empty()) return false;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) return true;
        // A missing leaf is normal (importing "<projects>/does_not_exist" must
        // report 404, not "outside the allowed roots"), so an existing PARENT
        // is enough to prove this reading of the bytes is the right one.
        const fs::path parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) return false;
        std::error_code parentEc;
        return fs::is_directory(parent, parentEc) && !parentEc;
    };

    const fs::path utf8Path = pathFromUtf8(text);
    if (resolves(utf8Path)) return utf8Path;
    try {
        const fs::path nativePath(text);  // ACP on Windows, bytes on POSIX
        if (resolves(nativePath)) return nativePath;
    } catch (const std::exception&) { /* fall through to the UTF-8 reading */ }
    return utf8Path;  // neither resolves: keep UTF-8 and let confinement decide
}

// Comparable form of a path: UTF-8 bytes (never the ACP narrow encoding, which
// would mangle a repository living under a CJK directory), separators folded to
// '/', trailing separators dropped. ASCII case folding on Windows only --
// folding on POSIX would widen containment (a/B and a/b differ there) -- and
// only for ASCII bytes, so multi-byte UTF-8 sequences are never touched.
std::string foldForCompare(const fs::path& path) {
    const auto u8 = path.generic_u8string();
    std::string value(reinterpret_cast<const char*>(u8.data()), u8.size());
    for (char& ch : value) {
        if (ch == static_cast<char>(92)) ch = '/';  // backslash
#ifdef _WIN32
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x80) {
            ch = static_cast<char>(std::tolower(byte));
        }
#endif
    }
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

// Canonicalize an existing directory; empty path on failure (fail closed).
fs::path canonicalDir(const fs::path& path) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(path, ec);
    if (ec || canon.empty()) {
        std::error_code ec2;
        canon = fs::canonical(path, ec2);
        if (ec2) return {};
    }
    return canon;
}

std::vector<fs::path> allowedImportRoots(const ProjectContext& ctx) {
    std::vector<fs::path> roots;
    const auto push = [&roots](const fs::path& candidate) {
        if (candidate.empty()) return;
        const fs::path canon = canonicalDir(candidate);
        if (canon.empty()) return;
        for (const auto& existing : roots) {
            if (foldForCompare(existing) == foldForCompare(canon)) return;
        }
        roots.push_back(canon);
    };
    push(ctx.projectRoot());
    push(ctx.sourceRoot());
    if (const char* env = std::getenv(kImportRootsEnv)) {
        const std::string list(env);
#ifdef _WIN32
        const char sep = ';';  // ':' is a drive-letter separator on Windows
#else
        const char sep = ':';
#endif
        std::string::size_type start = 0;
        while (start <= list.size()) {
            const auto end = list.find(sep, start);
            const std::string piece = list.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (!piece.empty()) push(pathFromUtf8(piece));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return roots;
}

// Resolve srcPath and verify containment. Returns an empty path when the input
// is malformed or escapes every allowed root.
fs::path confineImportSource(const ProjectContext& ctx, const std::string& rawPath) {
    if (rawPath.empty()) return {};
    // Reject UNC / network paths outright: containment cannot be reasoned
    // about across a remote namespace.
    if (rawPath.rfind("\\\\", 0) == 0 || rawPath.rfind("//", 0) == 0) return {};
#ifndef _WIN32
    // A Windows drive-letter path is meaningless (and unresolvable) here.
    if (rawPath.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(rawPath[0])) && rawPath[1] == ':') {
        return {};
    }
#endif
    std::string normalized = rawPath;
    for (char& ch : normalized) {
        if (ch == static_cast<char>(92)) ch = '/';
    }

    const auto roots = allowedImportRoots(ctx);
    const fs::path candidate = pathFromRequest(normalized);
    if (candidate.empty()) return {};

    // A relative input is tried against each allowed root in order (projects/
    // first, then the engine source root, then operator-configured roots), so
    // "my_project" and "tests/projects/first_vn" both work without the caller
    // ever naming an absolute path. The first existing directory wins, and its
    // canonical form is re-checked for containment (".." or a symlink inside
    // the root can still point outside it).
    if (candidate.is_relative()) {
        for (const auto& root : roots) {
            std::error_code dirEc;
            const fs::path resolved = root / candidate;
            if (!fs::is_directory(resolved, dirEc) || dirEc) continue;
            const fs::path canon = canonicalDir(resolved);
            if (canon.empty()) continue;
            const std::string target = foldForCompare(canon);
            const std::string prefix = foldForCompare(root);
            if (target == prefix || target.rfind(prefix + "/", 0) == 0) return canon;
        }
        return {};
    }

    std::error_code ec;
    const fs::path canon = canonicalDir(fs::absolute(candidate, ec));
    if (ec || canon.empty()) return {};

    const std::string target = foldForCompare(canon);
    for (const auto& root : roots) {
        const std::string prefix = foldForCompare(root);
        if (prefix.empty()) continue;
        if (target == prefix) return canon;                  // the root itself
        if (target.rfind(prefix + "/", 0) == 0) return canon;  // under the root
    }
    return {};
}

bool validProjectName(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

} // namespace

ServiceResult ProjectService::listTemplates() {
    Json out = Json::array();
    const fs::path root = m_ctx.templatesRoot();
    std::ifstream mf(root / "manifest.json");
    if (mf) {
        try {
            auto m = Json::parse(mf);
            if (m.contains("templates") && m["templates"].is_array()) {
                out = m["templates"];
            }
        } catch (const std::exception&) {
            out = Json::array();
        }
    }
    if (out.empty()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec || !e.is_directory()) continue;
            out.push_back({{"id", e.path().filename().string()},
                           {"name", e.path().filename().string()},
                           {"description", ""},
                           {"dir", e.path().filename().string()}});
        }
    }
    return ServiceResult::ok(out);
}

ServiceResult ProjectService::create(const std::string& templateId,
                                     const std::string& name) {
  try {
    return createImpl(templateId, name);
  } catch (const std::exception& ex) {
    return ServiceResult::err(500, std::string("create failed: ") + ex.what());
  }
}

ServiceResult ProjectService::createImpl(const std::string& templateId,
                                         const std::string& name) {
    if (name.empty()) return ServiceResult::err(400, "Missing project name");
    const bool known = templateId == "blank" || templateId == "basic" ||
                       templateId == "live2d" || templateId == "kag3" ||
                       templateId == "showcase" || templateId.empty();
    if (!known) return ServiceResult::err(400, "Unknown template");
    if (!validProjectName(name)) return ServiceResult::err(400, "Invalid project name");

    fs::path projectsRoot = m_ctx.projectRoot();
    std::error_code ec;
    fs::create_directories(projectsRoot, ec);

    fs::path dest = projectsRoot / name;
    if (fs::exists(dest, ec)) return ServiceResult::err(409, "Project already exists");

    const std::string dir = templateId.empty() ? "basic" : templateId;
    const std::string safeDir = [&] {
        std::string n = dir;
        for (auto& ch : n) if (ch == char(92)) ch = '/';
        return n;
    }();
    if (safeDir.find("..") != std::string::npos || safeDir.front() == '/' ||
        safeDir.find(':') != std::string::npos) {
        return ServiceResult::err(400, "Template not found");
    }
    fs::path src = m_ctx.templatesRoot() / safeDir;
    if (!fs::exists(src, ec)) return ServiceResult::err(400, "Template not found");

    std::error_code copyEc;
    fs::copy(src, dest, fs::copy_options::recursive |
                            fs::copy_options::overwrite_existing, copyEc);
    if (copyEc) return ServiceResult::err(500, "Failed to copy template");

    try {
        fs::path metaFile = dest / "caesura.project.json";
        if (fs::exists(metaFile, ec)) {
            std::ifstream metaIn(metaFile);
            std::ostringstream metaBuf;
            metaBuf << metaIn.rdbuf();
            auto meta = Json::parse(metaBuf.str());
            meta["name"] = name;
            meta["template"] = templateId.empty() ? "basic" : templateId;
            meta["created"] = nowIsoUtc();
            meta["modified"] = meta["created"];
            std::ofstream out(metaFile, std::ios::trunc);
            out << meta.dump(2);
        }
    } catch (const std::exception&) { /* best-effort */ }

    return ServiceResult::ok({{"ok", true}, {"path", dest.string()}});
}

ServiceResult ProjectService::duplicate(const std::string& src,
                                        const std::string& name) {
    if (src.empty() || name.empty()) return ServiceResult::err(400, "Missing srcPath/name");
    if (!validProjectName(name)) return ServiceResult::err(400, "Invalid project name");

    fs::path projectsRoot = m_ctx.projectRoot();
    std::error_code ec;
    const std::string norm = [&] {
        std::string safe = name;
        for (auto& ch : safe) if (ch == char(92)) ch = '/';
        return safe;
    }();
    fs::path srcDir = projectsRoot / norm;
    // src may be "projects/<name>" reference or bare <name>
    std::string srcName = src;
    const std::string prefix = "projects/";
    if (srcName.rfind(prefix, 0) == 0) srcName = srcName.substr(prefix.size());
    if (!validProjectName(srcName)) return ServiceResult::err(404, "Source project not found");
    fs::path srcPath = projectsRoot / srcName;
    if (!fs::exists(srcPath, ec) || !fs::is_directory(srcPath, ec)) {
        return ServiceResult::err(404, "Source project not found");
    }
    fs::path dest = projectsRoot / norm;
    if (fs::exists(dest, ec)) return ServiceResult::err(409, "Project already exists");

    std::error_code copyEc;
    fs::copy(srcPath, dest, fs::copy_options::recursive |
                                fs::copy_options::overwrite_existing, copyEc);
    if (copyEc) return ServiceResult::err(500, "Failed to duplicate");
    return ServiceResult::ok({{"ok", true}, {"path", dest.string()}});
}

ServiceResult ProjectService::importProject(const std::string& srcPath,
                                            const std::string& name) {
    if (srcPath.empty() || name.empty()) return ServiceResult::err(400, "Missing srcPath/name");
    if (!validProjectName(name)) return ServiceResult::err(400, "Invalid project name");

    // [Sprint 1 t4] Confine the import source BEFORE touching the filesystem:
    // srcPath used to be ANY absolute path, which turned this endpoint into an
    // arbitrary-directory read/copy primitive. Confinement runs first so an
    // out-of-root path cannot be used as an existence oracle (403 regardless of
    // whether it exists).
    const fs::path src = confineImportSource(m_ctx, srcPath);
    if (src.empty()) {
        return ServiceResult::err(403,
            "Source directory is outside the allowed import roots "
            "(projects/, the engine source root, or CAESURA_EDITOR_IMPORT_ROOTS)");
    }
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) {
        return ServiceResult::err(404, "Source directory not found");
    }
    if (!fs::exists(src / "story.ks", ec) && !fs::exists(src / "entry.lua", ec)) {
        return ServiceResult::err(400, "Not a Caesura project (missing story.ks/entry.lua)");
    }
    fs::path projectsRoot = m_ctx.projectRoot();
    std::error_code mkEc;
    fs::create_directories(projectsRoot, mkEc);
    fs::path dest = projectsRoot / name;
    if (fs::exists(dest, ec)) return ServiceResult::err(409, "Project already exists");
    // A source that CONTAINS the destination (e.g. importing the projects root
    // itself) would make fs::copy recurse into its own output.
    {
        std::error_code selfEc;
        const fs::path destParentCanon = canonicalDir(dest.parent_path());
        const std::string srcFolded = foldForCompare(src);
        const std::string destParentFolded =
            destParentCanon.empty() ? std::string() : foldForCompare(destParentCanon);
        (void)selfEc;
        if (!destParentFolded.empty() &&
            (destParentFolded == srcFolded ||
             destParentFolded.rfind(srcFolded + "/", 0) == 0)) {
            return ServiceResult::err(400,
                "Source directory contains the import destination");
        }
    }

    std::error_code copyEc;
    fs::copy(src, dest, fs::copy_options::recursive |
                            fs::copy_options::overwrite_existing, copyEc);
    if (copyEc) return ServiceResult::err(500, "Failed to import project");
    return ServiceResult::ok({{"ok", true}, {"path", dest.string()}});
}

ServiceResult ProjectService::list() {
    Json out = Json::array();
    try {
        fs::path projectsRoot = m_ctx.projectRoot();
        std::error_code ec;
        if (!fs::exists(projectsRoot, ec)) {
            fs::create_directories(projectsRoot, ec);
            return ServiceResult::ok(out);
        }
        for (const auto& e : fs::directory_iterator(projectsRoot, ec)) {
            if (ec) break;
            std::error_code de;
            if (!e.is_directory(de) || de) continue;
            const std::string name = e.path().filename().string();
            std::error_code te;
            const auto t = fs::last_write_time(e.path(), te);
            Json entry = {{"path", ("projects/" + name)},
                          {"name", name},
                          {"template", ""},
                          {"modified", te ? std::to_string(static_cast<long long>(t.time_since_epoch().count())) : ""}};
            fs::path metaFile = e.path() / "caesura.project.json";
            std::error_code me;
            if (fs::exists(metaFile, me)) {
                try {
                    std::ifstream in(metaFile);
                    std::ostringstream buf;
                    buf << in.rdbuf();
                    auto meta = Json::parse(buf.str());
                    if (meta.contains("template") && meta["template"].is_string())
                        entry["template"] = meta["template"];
                } catch (const std::exception&) { /* keep defaults */ }
            }
            out.push_back(std::move(entry));
        }
    } catch (const std::exception&) {
        return ServiceResult::ok(Json::array());
    }
    return ServiceResult::ok(out);
}

ServiceResult ProjectService::metaGet(const std::string& path) {
    // path must be "projects/<name>" (confined) -- reject everything else.
    const std::string prefix = "projects/";
    std::string norm = path;
    if (norm.find("..") != std::string::npos || norm.empty()) {
        return ServiceResult::err(400, "Invalid path");
    }
    if (!norm.starts_with(prefix)) return ServiceResult::err(400, "Invalid path");
    const std::string name = norm.substr(prefix.size());
    if (!validProjectName(name)) return ServiceResult::err(400, "Invalid path");

    const fs::path projectDir = m_ctx.projectRoot() / name;
    std::error_code ec;
    if (!fs::exists(projectDir, ec)) return ServiceResult::err(404, "Project not found");

    Json meta = defaultProjectMeta(name);
    bool inferred = true;
    const fs::path metaFile = projectDir / "caesura.project.json";
    std::error_code me;
    if (fs::exists(metaFile, me)) {
        try {
            std::ifstream in(metaFile);
            std::ostringstream buf;
            buf << in.rdbuf();
            const Json stored = Json::parse(buf.str());
            overlayStringMeta(meta, stored);
            // Capability requirements are authored data, not host support
            // facts. Keep even malformed declarations for the CLI to reject.
            if (stored.contains("capabilities")) meta["capabilities"] = stored["capabilities"];
            inferred = false;
        } catch (const std::exception&) { /* keep defaults */ }
    }
    return ServiceResult::ok({{"ok", true},
                              {"path", projectDir.string()},
                              {"inferred", inferred},
                              {"meta", std::move(meta)}});
}

ServiceResult ProjectService::metaSave(const std::string& path, Json meta) {
    // Validation parity with the original handler (smoke covers it):
    // language is a closed set; description is bounded free text.
    if (meta.contains("language") && meta["language"].is_string()) {
        const std::string lang = meta["language"].get<std::string>();
        if (lang != "zh" && lang != "en" && lang != "ja") {
            return ServiceResult::err(400, "language must be one of zh/en/ja");
        }
    } else if (meta.contains("language") && !meta["language"].is_null()) {
        return ServiceResult::err(400, "language must be a string");
    }
    if (meta.contains("description") && meta["description"].is_string() &&
        meta["description"].get<std::string>().size() > 2000) {
        return ServiceResult::err(400, "description too long (max 2000)");
    }
    // same path confinement as metaGet
    const std::string prefix = "projects/";
    std::string norm = path;
    if (norm.find("..") != std::string::npos || norm.empty() ||
        !norm.starts_with(prefix)) {
        return ServiceResult::err(400, "Invalid path");
    }
    const std::string name = norm.substr(prefix.size());
    if (!validProjectName(name)) return ServiceResult::err(400, "Invalid path");

    const fs::path projectDir = m_ctx.projectRoot() / name;
    std::error_code ec;
    if (!fs::exists(projectDir, ec)) return ServiceResult::err(404, "Project not found");

    Json final_meta = defaultProjectMeta(name);
    // This existing endpoint edits string metadata, not capability policy.
    // In particular, an old client must not replace or clear disk requirements.
    if (meta.is_object()) meta.erase("capabilities");
    overlayStringMeta(final_meta, meta);
    const fs::path metaFile = projectDir / "caesura.project.json";
    try {
        if (fs::exists(metaFile)) {
            std::ifstream in(metaFile, std::ios::binary);
            if (!in) return ServiceResult::err(500, "Failed to read existing metadata");
            const Json stored = Json::parse(in);
            if (in.bad() || !stored.is_object())
                return ServiceResult::err(500, "Failed to read existing metadata");
            // Preserve any JSON value, including invalid types/null. Validation
            // belongs to the capability checker and cannot be bypassed here.
            if (stored.contains("capabilities"))
                final_meta["capabilities"] = stored["capabilities"];
        }
    } catch (const std::exception&) {
        // Do not truncate a file whose declaration we could not preserve.
        return ServiceResult::err(500, "Failed to read existing metadata");
    }
    try {
        const std::string now = nowIsoUtc();
        final_meta["modified"] = now;
        const std::string serialized = final_meta.dump(2);
        std::ofstream out(metaFile, std::ios::trunc);
        out << serialized;
        out.flush();
        if (!out) return ServiceResult::err(500, "Failed to save metadata");
    } catch (const std::exception&) {
        return ServiceResult::err(500, "Failed to save metadata");
    }
    return ServiceResult::ok({{"ok", true},
                              {"path", projectDir.string()},
                              {"meta", std::move(final_meta)}});
}

} // namespace service
} // namespace rpc
} // namespace Caesura

#include "AssetManager.h"
#include "DirAssetProvider.h"
#include "AssetDirectoryValidation.h"
#include <set>
#include <cstdio>
#include <memory>

namespace Caesura {

AssetManager::~AssetManager() {
    shutdown();
}

void AssetManager::init() {
    if (m_initialized) return;

    m_chain.addProvider(std::make_unique<Caesura::DirAssetProvider>(""));
    m_chain.addProvider(std::make_unique<Caesura::DirAssetProvider>("assets"));

    m_initialized = true;
    printf("[AssetManager] Initialized (dir providers).\n");
}

void AssetManager::addProvider(std::unique_ptr<Caesura::IAssetProvider> provider) {
    m_chain.addProvider(std::move(provider));
}

void AssetManager::shutdown() {
    m_initialized = false;
    m_chain.clear();
}

std::vector<uint8_t> AssetManager::read(const std::string& path) {
    if (!m_initialized) return {};
    return m_chain.read(path);
}

bool AssetManager::exists(const std::string& path) {
    if (!m_initialized) return false;
    return m_chain.exists(path);
}

std::vector<uint8_t> AssetManager::readAsset(const std::string& path, size_t maxBytes) {
    if (!m_initialized || maxBytes == 0 || path.empty() || path.front() == '/'
        || path.find('\0') != std::string::npos || path.find('\\') != std::string::npos
        || path.find(':') != std::string::npos || path.find("..") != std::string::npos) {
        return {};
    }
    for (const auto& provider : m_chain.providers()) {
        try {
            if (!provider->exists(path)) continue;
            auto bytes = provider->read(path);
            if (bytes.size() > maxBytes) return {};
            return bytes;
        } catch (...) {
            // An uncertain or failed selected source cannot authorize a read
            // from a different layer during transaction preparation.
            return {};
        }
    }
    return {};
}

AssetDirectoryResult AssetManager::listDirectory(const std::string& directory,size_t maxEntries,size_t maxNameBytes) {
    std::string relative;
    if(!AssetDirectoryDetail::limits(maxEntries,maxNameBytes))return {AssetDirectoryStatus::LimitExceeded,{}};
    if(!AssetDirectoryDetail::normalize(directory,relative))return {AssetDirectoryStatus::InvalidPath,{}};
    if(!m_initialized)return {AssetDirectoryStatus::Unsupported,{}};
    try {
        std::set<std::string> names;size_t bytes=0;
        // Same priority order as readAsset. Deduplication changes no provider
        // registration or read authority. Any incomplete provider invalidates
        // completeness of the union; never disguise that as an empty folder.
        for(const auto& provider:m_chain.providers()) {
            if(!provider)return {AssetDirectoryStatus::IoError,{}};
            auto result=provider->listDirectory(relative,maxEntries,maxNameBytes);
            if(result.status!=AssetDirectoryStatus::Complete)return {result.status,{}};
            for(const auto& name:result.files) {
                if(!AssetDirectoryDetail::leaf(name))return {AssetDirectoryStatus::InvalidPath,{}};
                if(names.count(name))continue;
                if(names.size()>=maxEntries||name.size()>maxNameBytes-bytes)return {AssetDirectoryStatus::LimitExceeded,{}};
                names.insert(name);bytes+=name.size();
            }
        }
        return {AssetDirectoryStatus::Complete,{names.begin(),names.end()}};
    }catch(...){return {AssetDirectoryStatus::IoError,{}};}
}

} // namespace Caesura

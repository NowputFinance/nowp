// Copyright (c) 2022 The Raptoreum developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/powcache.h>
#include <fs.h>
#include <primitives/block.h>
#include <streams.h>
#include <hash.h>
#include <sync.h>
#include <util/system.h>

CPowCache* CPowCache::instance = nullptr;
RecursiveMutex cs_pow;

CPowCache& CPowCache::Instance()
{
    if (CPowCache::instance == nullptr)
    {
        int64_t maxElements  = gArgs.GetIntArg("-powcachemaxelements",  DEFAULT_POWCACHE_MAX_ELEMENTS);
        bool    validate     = gArgs.GetBoolArg("-powcachevalidate",    DEFAULT_POWCACHE_VALIDATE);
        int64_t saveInterval = gArgs.GetIntArg("-powcachesaveinterval", DEFAULT_POWCACHE_SAVE_INTERVAL);

        maxElements = maxElements <= 0 ? DEFAULT_POWCACHE_MAX_ELEMENTS : maxElements;

        CPowCache::instance = new CPowCache(maxElements, validate, saveInterval);
    }
    return *instance;
}

void CPowCache::Load()
{
    fs::path path = gArgs.GetDataDirNet() / "powcache.dat";
    CAutoFile file(fsbridge::fopen(path, "rb"), SER_DISK, CURRENT_VERSION);
    if (file.IsNull()) {
        LogPrintf("PowCache: Unable to load file.  Cache is empty.\n");
        return;
    }
    LOCK(cs_pow);
    clear();
    Unserialize(file);
    nSavedSize = size();
    LogPrintf("PowCache: Loaded: %d elements\n", size());
}

void CPowCache::Save()
{
    fs::path path = gArgs.GetDataDirNet() / "powcache.dat";
    CAutoFile file(fsbridge::fopen(path, "wb"), SER_DISK, CURRENT_VERSION);
    if (file.IsNull()) {
        LogPrintf("PowCache: Unable to save file.\n");
        return;
    }
    LOCK(cs_pow);
    Serialize(file);
    nSavedSize = size();
    LogPrintf("PowCache: Saved: %d elements\n", size());
}

void CPowCache::DoMaintenance()
{
    LOCK(cs_pow);
    // If cache has grown enough, save it:
    if (size() - nSavedSize >= nSaveInterval) {
        Save();
    }
}

CPowCache::CPowCache(int64_t maxElements, bool validate, int64_t saveInterval)
    : unordered_lru_cache<uint256, uint256, std::hash<uint256>>(maxElements)
    , nVersion(CURRENT_VERSION)
    , nSavedSize(0)
    , bValidate(validate)
    , nSaveInterval(saveInterval)
{
    if (bValidate) LogPrintf("PowCache: Validation and auto correction enabled\n");
}

CPowCache::~CPowCache()
{
}

void CPowCache::Clear()
{
    clear();
}

void CPowCache::CheckAndRemove()
{
}

std::string CPowCache::ToString() const
{
    std::ostringstream info;
    info << "PowCache: elements: " << size();
    return info.str();
}

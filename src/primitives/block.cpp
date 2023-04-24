// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <primitives/powcache.h>
#include <util/strencodings.h>
#include <hash.h>
#include <tinyformat.h>

uint256 CBlockHeader::GetHash() const
{
    CBlockHeader tmp(*this);
    tmp.nFlags = 0;
    return SerializeHash(tmp);
}

uint256 CBlockHeader::ComputeHash() const
{
    return HashGR(BEGIN(nVersion), END(nNonce), hashPrevBlock);
}

uint256 CBlockHeader::GetPOWHash(bool readCache) const
{
    CPowCache& cache(CPowCache::Instance());

    uint256 headerHash = GetHash();
    uint256 powHash;
    bool found = false;

    if (readCache) {
        found = cache.get(headerHash, powHash);
    }

    if (!found || cache.GetValidate()) {
        uint256 powHash2 = ComputeHash();
        if (found && powHash2 != powHash) {
            // We cannot use the loggers at this level
            std::cerr << "PowCache failure: headerHash: " << headerHash.ToString() << ", from cache: " << powHash.ToString() << ", computed: " << powHash2.ToString() << ", correcting" << std::endl;
        }
        cache.insert(headerHash, powHash2);
    }
    return powHash;
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, nFlags=%08x, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        nFlags, vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}

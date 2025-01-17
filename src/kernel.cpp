// Copyright (c) 2012-2023 The Nowp developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel.h>
#include <chainparams.h>
#include <validation.h>
#include <streams.h>
#include <timedata.h>
#include <bignum.h>
#include <txdb.h>
#include <consensus/validation.h>
#include <validation.h>
#include <random.h>
#include <script/interpreter.h>

#include <index/txindex.h>

#include <boost/assign/list_of.hpp>

using namespace std;

// Switch time for new BIPs from bitcoin 0.16.x
const uint32_t nBTC16BIPsSwitchTime           = 1677525510; 
const uint32_t nBTC16BIPsTestSwitchTime       = 1677525510; 

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
    ( 1000000, 0x0e00670bu )
    ;

static std::map<int, unsigned int> mapStakeModifierTestnetCheckpoints =
    boost::assign::map_list_of
    ( 1000000, 0x0e00670bu )
    ;


bool IsBTC16BIPsEnabled(uint32_t nTimeTx)
{
    bool fTestNet = Params().NetworkIDString() != CBaseChainParams::MAIN;
    return (nTimeTx >= (fTestNet? nBTC16BIPsTestSwitchTime : nBTC16BIPsSwitchTime));
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (Params().GetConsensus().nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
    vector<pair<int64_t, uint256> >& vSortedByTimestamp,
    map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev,
    const CBlockIndex** pindexSelected,
    CChainState& chainstate)
{
    bool fSelected = false;
    arith_uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    for (const auto& item : vSortedByTimestamp)
    {
/*
        if (!chainstate.BlockIndex().count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString());
        const CBlockIndex* pindex = pindexSelected.BlockIndex()[item.second];
*/
        const CBlockIndex* pindex = chainstate.m_blockman.LookupBlockIndex(item.second);
        if (!pindex)
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString());

        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        uint256 hashProof = pindex->IsProofOfStake()? pindex->hashProofOfStake : pindex->GetBlockHash();
        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss));
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printstakemodifier", false))
        LogPrintf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexCurrent, uint64_t &nStakeModifier, bool& fGeneratedStakeModifier, CChainState& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CBlockIndex* pindexPrev = pindexCurrent->pprev;
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");
    if (gArgs.GetBoolArg("-debug", false))
        LogPrintf("ComputeNextStakeModifier: prev modifier=0x%016x time=%s epoch=%u\n", nStakeModifier, FormatISO8601DateTime(nModifierTime), (unsigned int)nModifierTime);
    if (nModifierTime / params.nModifierInterval >= pindexPrev->GetBlockTime() / params.nModifierInterval)
    {
        if (gArgs.GetBoolArg("-debug", false))
            LogPrintf("ComputeNextStakeModifier: no new interval keep current modifier: pindexPrev nHeight=%d nTime=%u\n", pindexPrev->nHeight, (unsigned int)pindexPrev->GetBlockTime());
        return true;
    }
    if (nModifierTime / params.nModifierInterval >= pindexCurrent->GetBlockTime() / params.nModifierInterval)
    {
        if (gArgs.GetBoolArg("-debug", false))
            LogPrintf("ComputeNextStakeModifier: no new interval keep current modifier: pindexCurrent nHeight=%d nTime=%u\n", pindexCurrent->nHeight, (unsigned int)pindexCurrent->GetBlockTime());
        return true;
    }

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * params.nModifierInterval / params.nStakeTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / params.nModifierInterval) * params.nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;

    // Shuffle before sort
    for(int i = vSortedByTimestamp.size() - 1; i > 1; --i)
    std::swap(vSortedByTimestamp[i], vSortedByTimestamp[GetRand(i)]);

    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end(), [] (const pair<int64_t, uint256> &a, const pair<int64_t, uint256> &b)
    {
        if (a.first != b.first)
            return a.first < b.first;
        // Timestamp equals - compare block hashes
        const uint32_t *pa = a.second.GetDataPtr();
        const uint32_t *pb = b.second.GetDataPtr();
        int cnt = 256 / 32;
        do {
            --cnt;
            if (pa[cnt] != pb[cnt])
                return pa[cnt] < pb[cnt];
        } while(cnt);
            return false; // Elements are equal
    });

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex, chainstate))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printstakemodifier", false))
            LogPrintf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n",
                nRound, FormatISO8601DateTime(nSelectionIntervalStop), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printstakemodifier", false))
    {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const auto& item : mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake()? "S" : "W");
        }
        LogPrintf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap);
    }
    if (gArgs.GetBoolArg("-debug", false))
        LogPrintf("ComputeNextStakeModifier: new modifier=0x%016x time=%s\n", nStakeModifierNew, FormatISO8601DateTime(pindexPrev->GetBlockTime()));

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// V0.5: Stake modifier used to hash for a stake kernel is chosen as the stake
// modifier that is (nStakeMinAge minus a selection interval) earlier than the
// stake, thus at least a selection interval later than the coin generating the
// kernel, as the generating coin is from at least nStakeMinAge ago.
static bool GetKernelStakeModifierV05(CBlockIndex* pindexPrev, unsigned int nTimeTx, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CBlockIndex* pindex = pindexPrev;
    nStakeModifierHeight = pindex->nHeight;
    nStakeModifierTime = pindex->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    if (nStakeModifierTime + params.nStakeMinAge - nStakeModifierSelectionInterval <= (int64_t) nTimeTx)
    {
        // Best block is still more than
        // (nStakeMinAge minus a selection interval) older than kernel timestamp
        if (fPrintProofOfStake)
            return error("GetKernelStakeModifier() : best block %s at height %d too old for stake",
                pindex->GetBlockHash().ToString(), pindex->nHeight);
        else
            return false;
    }
    // loop to find the stake modifier earlier by 
    // (nStakeMinAge minus a selection interval)
    while (nStakeModifierTime + params.nStakeMinAge - nStakeModifierSelectionInterval >(int64_t) nTimeTx)
    {
        if (!pindex->pprev)
        {   // reached genesis block; should not happen
            return error("GetKernelStakeModifier() : reached genesis block");
        }
        pindex = pindex->pprev;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// V0.3: Stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifierV03(CBlockIndex* pindexPrev, uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake, CChainState& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();
    nStakeModifier = 0;

    const CBlockIndex* pindexFrom = chainstate.m_blockman.LookupBlockIndex(hashBlockFrom);
    if (!pindexFrom)
        return error("GetKernelStakeModifier() : block not indexed");

    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();


    // we need to iterate index forward but we cannot depend on chainActive.Next()
    // because there is no guarantee that we are checking blocks in active chain.
    // So, we construct a temporary chain that we will iterate over.
    // pindexFrom - this block contains coins that are used to generate PoS
    // pindexPrev - this is a block that is previous to PoS block that we are checking, you can think of it as tip of our chain
    std::vector<CBlockIndex*> tmpChain;
    int32_t nDepth = pindexPrev->nHeight - (pindexFrom->nHeight-1); // -1 is used to also include pindexFrom
    tmpChain.reserve(nDepth);
    CBlockIndex* it = pindexPrev;
    for (int i=1; i<=nDepth && !chainstate.m_chain.Contains(it); i++) {
        tmpChain.push_back(it);
        it = it->pprev;
    }
    std::reverse(tmpChain.begin(), tmpChain.end());
    size_t n = 0;

    const CBlockIndex* pindex = pindexFrom;
    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        const CBlockIndex* old_pindex = pindex;
        pindex = (!tmpChain.empty() && pindex->nHeight >= tmpChain[0]->nHeight - 1)? tmpChain[n++] : chainstate.m_chain.Next(pindex);
        if (n > tmpChain.size() || pindex == NULL) // check if tmpChain[n+1] exists
        {   // reached best block; may happen if node is behind on block chain
            if (fPrintProofOfStake || (old_pindex->GetBlockTime() + params.nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    old_pindex->GetBlockHash().ToString(), old_pindex->nHeight, hashBlockFrom.ToString());
            else
                return false;
        }
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// Get the stake modifier specified by the protocol to hash for a stake kernel
static bool GetKernelStakeModifier(CBlockIndex* pindexPrev, uint256 hashBlockFrom, unsigned int nTimeTx, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake, CChainState& chainstate)
{
    return GetKernelStakeModifierV05(pindexPrev, nTimeTx, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake);

}

// nowp kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: 
//       (v0.5) uses dynamic stake modifier around 21 days before the kernel,
//              versus static stake modifier about 9 days after the staked
//              coin (txPrev) used in v0.3
//       (v0.3) scrambles computation to make it very difficult to precompute
//              future proof-of-stake at the time of the coin's confirmation
//       (v0.2) nBits (deprecated): encodes all past block timestamps
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of 
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(unsigned int nBits, CBlockIndex* pindexPrev, const CBlockHeader& blockFrom, unsigned int nTxPrevOffset, const CTransactionRef& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, bool fPrintProofOfStake, CChainState& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();
    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();

    if (nTimeTx < (txPrev->nTime? txPrev->nTime : nTimeBlockFrom))  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    if (nTimeBlockFrom + params.nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    int64_t nValueIn = txPrev->vout[prevout.n].nValue;
    // v0.3 protocol kernel hash weight starts from 0 at the 30-day min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
    int64_t nTimeWeight = min((int64_t)nTimeTx - (txPrev->nTime? txPrev->nTime : nTimeBlockFrom), params.nStakeMaxAge) - params.nStakeMinAge;
    CBigNum bnCoinDayWeight = CBigNum(nValueIn) * nTimeWeight / COIN / (24 * 60 * 60);
    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;
    if (!GetKernelStakeModifier(pindexPrev, blockFrom.GetHash(), nTimeTx, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake, chainstate))
        return false;
    ss << nStakeModifier;

    ss << nTimeBlockFrom << nTxPrevOffset << (txPrev->nTime? txPrev->nTime : nTimeBlockFrom) << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss);
    if (fPrintProofOfStake)
    {
        const CBlockIndex* pindexTmp = chainstate.m_blockman.LookupBlockIndex(blockFrom.GetHash());
        LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            FormatISO8601DateTime(nStakeModifierTime),
            pindexTmp->nHeight,
            FormatISO8601DateTime(blockFrom.GetBlockTime()));

        LogPrintf("CheckStakeKernelHash() : check protocol=%s modifier=0x%016x nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, (txPrev->nTime? txPrev->nTime : nTimeBlockFrom), prevout.n, nTimeTx,
            hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (CBigNum(hashProofOfStake) > bnCoinDayWeight * bnTargetPerCoinDay)
        return false;
    if (gArgs.GetBoolArg("-debug", false) && !fPrintProofOfStake)
    {
        const CBlockIndex* pindexTmp = chainstate.m_blockman.LookupBlockIndex(blockFrom.GetHash());
        LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            FormatISO8601DateTime(nStakeModifierTime),
            pindexTmp->nHeight,
            FormatISO8601DateTime(blockFrom.GetBlockTime()));

        LogPrintf("CheckStakeKernelHash() : pass protocol=%s modifier=0x%016x nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            "0.3", nStakeModifier, nTimeBlockFrom, nTxPrevOffset, (txPrev->nTime? txPrev->nTime : nTimeBlockFrom), prevout.n, nTimeTx, hashProofOfStake.ToString());
    }
    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(BlockValidationState &state, CBlockIndex* pindexPrev, const CTransactionRef& tx, unsigned int nBits, uint256& hashProofOfStake, unsigned int nTimeTx, CChainState& chainstate)
{
    if (!tx->IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx->GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx->vin[0];

    // Transaction index is required to get to block header
    if (!g_txindex)
        return error("CheckProofOfStake() : transaction index not available");

    // Get transaction index for the previous transaction
    CDiskTxPos postx;
    if (!g_txindex->FindTxPosition(txin.prevout.hash, postx))
        return error("CheckProofOfStake() : tx index not found");  // tx index not found

    // Read txPrev and header of its block
    CBlockHeader header;
    CTransactionRef txPrev;
    auto it = g_txindex->cachedTxs.find(txin.prevout.hash);
    if (it != g_txindex->cachedTxs.end()) {
        header = it->second.first;
        txPrev = it->second.second;
    } else {
        CAutoFile file(node::OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
        try {
            file >> header;
            fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
            file >> txPrev;
        } catch (std::exception &e) {
            return error("%s() : deserialize or I/O error in CheckProofOfStake()", __PRETTY_FUNCTION__);
        }
        //g_txindex->cachedTxs[txin.prevout.hash] = std::pair(header,txPrev);
    }

    if (txPrev->GetHash() != txin.prevout.hash)
        return error("%s() : txid mismatch in CheckProofOfStake()", __PRETTY_FUNCTION__);

    // Verify signature
    {
        int nIn = 0;
        const CTxOut& prevOut = txPrev->vout[tx->vin[nIn].prevout.n];
        TransactionSignatureChecker checker(&(*tx), nIn, prevOut.nValue, PrecomputedTransactionData(*tx), MissingDataBehavior(1));

        if (!VerifyScript(tx->vin[nIn].scriptSig, prevOut.scriptPubKey, &(tx->vin[nIn].scriptWitness), SCRIPT_VERIFY_P2SH, checker, nullptr))
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "invalid-pos-script", strprintf("%s: VerifyScript failed on coinstake %s", __func__, tx->GetHash().ToString()));
    }

    if (!CheckStakeKernelHash(nBits, pindexPrev, header, postx.nTxOffset + CBlockHeader::NORMAL_SERIALIZE_SIZE, txPrev, txin.prevout, nTimeTx, hashProofOfStake, gArgs.GetBoolArg("-debug", false), chainstate))
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "check-kernel-failed", strprintf("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx->GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert (pindex->pprev || pindex->GetBlockHash() == Params().GetConsensus().hashGenesisBlock);
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << pindex->hashProofOfStake << pindex->nStakeModifier;
    arith_uint256 hashChecksum = UintToArith256(Hash(ss));
    hashChecksum >>= (256 - 32);
    return hashChecksum.GetLow64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    bool fTestNet = Params().NetworkIDString() == CBaseChainParams::TESTNET;
    if (fTestNet && mapStakeModifierTestnetCheckpoints.count(nHeight))
        return nStakeModifierChecksum == mapStakeModifierTestnetCheckpoints[nHeight];

    if (!fTestNet && mapStakeModifierCheckpoints.count(nHeight))
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];

    return true;
}

bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    return (HowSuperMajority(minVersion, pstart, nRequired, nToCheck) >= nRequired);
}

unsigned int HowSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; pstart = pstart->pprev )
    {
        if (!pstart->IsProofOfStake())
            continue;

        if (pstart->nVersion >= minVersion)
            ++nFound;

        i++;
    }
    return nFound;
}

// nowp: entropy bit for stake modifier if chosen by modifier
unsigned int GetStakeEntropyBit(const CBlock& block)
{
    unsigned int nEntropyBit = 0;

    nEntropyBit = UintToArith256(block.GetHash()).GetLow64() & 1llu;// last bit of block hash
    if (gArgs.GetBoolArg("-printstakemodifier", false))
        LogPrintf("GetStakeEntropyBit(v0.4+): nTime=%u hashBlock=%s entropybit=%d\n", block.nTime, block.GetHash().ToString(), nEntropyBit);
    return nEntropyBit;
}


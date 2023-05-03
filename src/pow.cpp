// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>

#include <bignum.h>
#include <chainparams.h>
#include <kernel.h>
#include <math.h>
#include <timedata.h>
#include <logging.h>


unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const Consensus::Params& params, bool fProofOfStake) {
    /* current difficulty formula, raptoreum - DarkGravity v3, written by Evan Duffield - evan@raptoreum.org */
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = params.DGWBlocksAvg;

    // make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
        return bnPowLimit.GetCompact();
    }

    const CBlockIndex *pindex = pindexLast;
    arith_uint512 bnPastTargetAvg;
	for (unsigned int nCountBlocks = 1; nCountBlocks <= nPastBlocks; nCountBlocks++) {
		arith_uint512 bnTarget = arith_uint512(arith_uint256().SetCompact(pindex->nBits));
		if (nCountBlocks == 1) {
			bnPastTargetAvg = bnTarget;
		} else {
			// NOTE: that's not an average really...
			bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
		}

		if(nCountBlocks != nPastBlocks) {
			assert(pindex->pprev); // should never fail
            pindex = pindex->pprev;
            while (pindex->IsProofOfStake() != fProofOfStake){
                pindex = pindex->pprev;
                assert(pindex); // should never fail
            }
		}
	}

	arith_uint512 bnNew(bnPastTargetAvg);
	int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
	// NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
		int64_t nTargetTimespan = nPastBlocks; 
    if (fProofOfStake)
        nTargetTimespan *= params.nStakeTargetSpacing;
    else
        nTargetTimespan *= params.nPowTargetSpacing;
    //NOTE: once PoS is active, block target spacing need to be doubled to maintain 720 block day (360 PoW, 360 PoS)
    if (pindexLast->nHeight > params.nPoSActivationHeight) 
        nTargetTimespan *= 2;

	if (nActualTimespan < nTargetTimespan/3)
		nActualTimespan = nTargetTimespan/3;
	if (nActualTimespan > nTargetTimespan*3)
		nActualTimespan = nTargetTimespan*3;

	// Retarget
	bnNew *= nActualTimespan;
	bnNew /= nTargetTimespan;
	arith_uint256 bnFinal = bnNew.trim256();
	if (bnFinal <= 0 || bnFinal > bnPowLimit) {
		bnFinal = bnPowLimit;
	}

	return bnFinal.GetCompact();
}

// for DIFF_BTC only!
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nTargetTimespan/4)
        nActualTimespan = params.nTargetTimespan/4;
    if (nActualTimespan > params.nTargetTimespan*4)
        nActualTimespan = params.nTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

static bool IsPosDGWenabled(const CBlockIndex* pindexLast,const Consensus::Params& params)
{
    static bool DGWenabled = false;

    if (DGWenabled) 
        return DGWenabled;

    const CBlockIndex* pindex = pindexLast;
    int count = 0;
    while (pindex)
    {
        pindex = pindex->pprev;
        //find the next PoS block
        while (pindex && pindex->IsProofOfStake() != true)
            pindex = pindex->pprev;
        
        if (!pindex)
            return false;

        count++;
        if (count > params.nPowDGWHeight + 1){
            DGWenabled = true;
            return DGWenabled;
        }
    }
    return false;
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, bool fProofOfStake, const Consensus::Params& params)
{
    if (pindexLast == nullptr)
        return UintToArith256(params.powLimit).GetCompact(); // genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == nullptr)
        return UintToArith256(params.bnInitialHashTarget).GetCompact(); // first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == nullptr)
        return UintToArith256(params.bnInitialHashTarget).GetCompact(); // second block

    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

    // rfc20
    int64_t nHypotheticalSpacing = pindexLast->GetBlockTime() - pindexPrev->GetBlockTime();
    if (!fProofOfStake && (nHypotheticalSpacing > nActualSpacing))
        nActualSpacing = nHypotheticalSpacing;

    // nowp: target change every block
    // nowp: retarget with exponential moving toward target spacing
    CBigNum bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    if (Params().NetworkIDString() != CBaseChainParams::REGTEST) {
        int64_t nTargetSpacing;

    if (fProofOfStake) {
        nTargetSpacing = params.nStakeTargetSpacing * 2;
    } else {
        nTargetSpacing = params.nPowTargetSpacing;
    }

    int64_t nInterval = params.nTargetTimespan / nTargetSpacing;
    //need 60 or more blocks to use DarkGravityWave
    if ((fProofOfStake && !IsPosDGWenabled(pindexPrev, params)) || (pindexLast->nHeight + 1 < params.nPowDGWHeight)) {
        bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
        bnNew /= ((nInterval + 1) * nTargetSpacing);
    } else {
        return DarkGravityWave(pindexPrev, params, fProofOfStake);
    }
}

    if (bnNew > CBigNum(params.powLimit))
        bnNew = CBigNum(params.powLimit);

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

bool CheckPOW(const CBlock& block, const Consensus::Params& consensusParams)
{
    if (block.IsProofOfStake())
        return true;

    if (!CheckProofOfWork(block.GetPOWHash(), block.nBits, consensusParams)) {
        std::string str = block.IsProofOfWork() ?  "POW" : "POS";
        LogPrintf("CheckPOW: CheckProofOfWork failed for %s, retesting without POW cache, block type %s\n", block.GetHash().ToString(), str);

        // Retest without POW cache in case cache was corrupted:
        return CheckProofOfWork(block.GetPOWHash(false), block.nBits, consensusParams);
    }
    return true;
}

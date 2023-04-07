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


unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const Consensus::Params& params) {
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
		}
	}

	arith_uint512 bnNew(bnPastTargetAvg);
	int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
	// NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
	int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

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
        nTargetSpacing = params.nStakeTargetSpacing;
    } else {
        nTargetSpacing = params.nStakeTargetSpacing;
    }

    int64_t nInterval = params.nTargetTimespan / nTargetSpacing;
    if (fProofOfStake || (pindexLast->nHeight + 1 < params.nPowDGWHeight)) {
        bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
        bnNew /= ((nInterval + 1) * nTargetSpacing);
    } else {
        return DarkGravityWave(pindexLast, false, params);
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

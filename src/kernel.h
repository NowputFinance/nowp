// Copyright (c) 2012-2023 The Nowp developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef NOWP_KERNEL_H
#define NOWP_KERNEL_H

#include <primitives/transaction.h> // CTransaction(Ref)

class CBlockIndex;
class BlockValidationState;
class CBlockHeader;
class CBlock;
class CChainState;


// MODIFIER_INTERVAL_RATIO:
// ratio of group interval length between the last group and the first group
static const int MODIFIER_INTERVAL_RATIO = 3;

// Whether a given block is subject to new BIPs from bitcoin 0.16.x
bool IsBTC16BIPsEnabled(uint32_t nTimeTx);

// Compute the hash modifier for proof-of-stake
bool ComputeNextStakeModifier(const CBlockIndex* pindexCurrent, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier, CChainState& chainstate);

// Check whether stake kernel meets hash target
// Sets hashProofOfStake on success return
bool CheckStakeKernelHash(unsigned int nBits, CBlockIndex* pindexPrev, const CBlockHeader& blockFrom, unsigned int nTxPrevOffset, const CTransactionRef& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, bool fPrintProofOfStake, CChainState& chainstate);

// Check kernel hash target and coinstake signature
// Sets hashProofOfStake on success return
bool CheckProofOfStake(BlockValidationState &state, CBlockIndex* pindexPrev, const CTransactionRef &tx, unsigned int nBits, uint256& hashProofOfStake, unsigned int nTimeTx, CChainState& chainstate);

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx);

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex);

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum);

// nowp: block version supermajority calculation
bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck);
unsigned int HowSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck);

// nowp: entropy bit for stake modifier if chosen by modifier
unsigned int GetStakeEntropyBit(const CBlock& block);

#endif // NOWP_KERNEL_H

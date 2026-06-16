// Copyright (c) 2026 The ZClassic developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .
#ifndef ZCL_BETA7_RELEASEFLAGS_H
#define ZCL_BETA7_RELEASEFLAGS_H

// Beta7 production rule: NFT file bytes are never sent on-chain. The legacy
// ZDC1 data-channel UI is available only to explicit developer builds.
#ifdef ZCL_ENABLE_LEGACY_DATACHANNEL_UI
static const bool ZCL_LEGACY_DATACHANNEL_UI = true;
#else
static const bool ZCL_LEGACY_DATACHANNEL_UI = false;
#endif

#endif // ZCL_BETA7_RELEASEFLAGS_H

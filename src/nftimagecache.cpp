// ============================================================================
// nftimagecache.cpp — BACK-COMPAT SHIM (Phase C1).
//
// All behavior now lives in src/contentengine.{h,cpp}. NFTImageCache is a thin
// alias-subclass of ContentEngine (see nftimagecache.h) whose ctor/dtor are
// inline, so this translation unit carries no logic of its own — it exists only
// so the build's source list (zcl-qt-wallet.pro, tests/tests.pro) stays valid
// without edits this pass. DRY: there is exactly ONE engine implementation.
// ============================================================================
#include "nftimagecache.h"

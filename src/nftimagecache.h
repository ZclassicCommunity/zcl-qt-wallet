// ============================================================================
// nftimagecache.h — BACK-COMPAT SHIM (Phase C1).
//
// The image cache has been generalized into the ONE streaming ContentEngine
// (src/contentengine.{h,cpp}) — see that header for the full streaming /
// chunked-Merkle / cancellation / content-addressed-cache contract.
//
// NFTImageCache is now a thin alias-subclass of ContentEngine so that existing
// call sites (mainwindow.cpp:3053/3124/3204, rpc.cpp, tests) and the forward
// declaration `class NFTImageCache;` in mainwindow.h keep compiling with ZERO
// edits this pass. The constructor forwards to ContentEngine. A typedef cannot
// satisfy a forward `class` declaration, so this is an (empty) subclass, not a
// typedef. There is NO duplicate implementation — DRY: one engine.
// ============================================================================
#ifndef NFTIMAGECACHE_H
#define NFTIMAGECACHE_H

#include "contentengine.h"

class NFTGalleryModel;

class NFTImageCache : public ContentEngine {
    Q_OBJECT
public:
    explicit NFTImageCache(NFTGalleryModel* model, QObject* parent = nullptr)
        : ContentEngine(model, parent) {}
    ~NFTImageCache() override = default;
};

#endif // NFTIMAGECACHE_H

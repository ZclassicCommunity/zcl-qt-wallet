// ============================================================================
// NFTGalleryDelegate — paints one NFT as a rounded card in the Collections
// gallery QListView (Phase C0). Native QPainter only; no HTML/web.
//
// Each card draws (top to bottom):
//   * a rounded card body (card #15171c) with a 1px hairline (#2a2d35);
//   * a square thumbnail area (inset #1d2027) — the model's QPixmap if ready,
//     else an animated shimmer placeholder while verifyState == pending;
//   * a privacy pill (green "Private" / amber "Public") reusing the tinted-SVG
//     mask technique from PrivacyBadgeDelegate;
//   * a verify badge (green check / red x / amber question) from the bundled
//     16px SVGs, tinted the same way;
//   * the name + collection caption.
//
// sizeHint is a fixed (168 x 208) device-independent card, multiplied by the
// device pixel ratio so it stays crisp on HiDPI. The tinted icons are rendered
// ONCE and cached (keyed by kind+px); paint() allocates no pixmaps on the hot
// path. Stateless w.r.t. data — all per-item state comes from the model roles.
// ============================================================================
#ifndef NFTGALLERYDELEGATE_H
#define NFTGALLERYDELEGATE_H

#include "precompiled.h"

#include <QStyledItemDelegate>
#include <QHash>
#include <QPixmap>

class NFTGalleryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit NFTGalleryDelegate(QObject* parent = nullptr);

    void  paint(QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    // The device-independent card footprint (before devicePixelRatio scaling).
    // Public so the unit test can assert the contract without painting.
    static QSize baseCardSize();

private:
    // Verify badge resource + tint color for a verifyState (0/1/2).
    static QString verifyIconResource(int verifyState);
    static QColor  verifyColor(int verifyState);

    // Privacy pill label + color (reuses the green/amber tokens).
    static QString privacyLabel(bool isPrivate);
    static QColor  privacyColor(bool isPrivate);

    // Tinted, cached SVG icon (alpha-mask -> SourceIn fill), keyed by
    // resource+color+px. Mirrors PrivacyBadgeDelegate::icon().
    const QPixmap& tintedIcon(const QString& resource, const QColor& color, int px) const;

    mutable QHash<QString, QPixmap> _iconCache;   // populated by const paint()
};

#endif // NFTGALLERYDELEGATE_H

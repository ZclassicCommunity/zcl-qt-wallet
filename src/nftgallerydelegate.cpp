// ============================================================================
// NFTGalleryDelegate implementation — see nftgallerydelegate.h for intent.
// Tokens mirror res/styles/dark.qss exactly:
//   app #0f1115  card #15171c  inset #1d2027  hairline #2a2d35
//   text #e6e6e6  dim #9aa0a6  private/green #1f7a1f  public/amber #d9822b
//   danger/red #c0392b
// ============================================================================
#include "nftgallerydelegate.h"
#include "nftgallerymodel.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QApplication>
#include <QSvgRenderer>
#include <QFontMetrics>
#include <QPalette>
#include <QStyleOptionViewItem>
#include <QModelIndex>
#include <QColor>
#include <QFont>
#include <QCryptographicHash>
#include <QByteArray>

namespace {
    const QColor kCard    ("#15171c");
    const QColor kInset   ("#1d2027");
    const QColor kHair    ("#2a2d35");
    const QColor kText    ("#e6e6e6");
    const QColor kDim     ("#9aa0a6");
    const QColor kGreen   ("#1f7a1f");
    const QColor kAmber   ("#d9822b");
    const QColor kRed     ("#c0392b");

    // Device-independent card geometry (px). sizeHint scales this by DPR.
    const int kCardW       = 168;
    const int kCardH       = 208;
    const int kPad         = 8;     // inner margin from the card edge
    const int kCardRadius  = 12;    // matches the qss card radius
    const int kThumbRadius = 8;     // matches the qss inset radius
    const int kVerifyPx    = 16;    // bundled badge SVGs are 16px
    const int kPillH       = 18;    // privacy pill height
}

NFTGalleryDelegate::NFTGalleryDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

QSize NFTGalleryDelegate::baseCardSize() {
    return QSize(kCardW, kCardH);
}

QString NFTGalleryDelegate::verifyIconResource(int verifyState) {
    switch (verifyState) {
        case 1:  return QStringLiteral(":/icons/res/icons/check.svg");
        case 2:  return QStringLiteral(":/icons/res/icons/x.svg");
        default: return QStringLiteral(":/icons/res/icons/question.svg");   // pending
    }
}

QColor NFTGalleryDelegate::verifyColor(int verifyState) {
    switch (verifyState) {
        case 1:  return kGreen;     // verified
        case 2:  return kRed;       // mismatch
        default: return kAmber;     // pending / unknown
    }
}

const QPixmap& NFTGalleryDelegate::tintedIcon(const QString& resource,
                                              const QColor& color, int px) const {
    const QString key = resource + "|" + color.name() + "|" + QString::number(px);
    auto it = _iconCache.find(key);
    if (it != _iconCache.end())
        return it.value();

    QPixmap pm(px, px);
    pm.fill(Qt::transparent);

    QSvgRenderer renderer(resource);
    if (renderer.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p, QRectF(0, 0, px, px));
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(QRect(0, 0, px, px), color);
        p.end();
    }
    // If QtSvg is unavailable the pixmap stays transparent (documented fallback).

    it = _iconCache.insert(key, pm);
    return it.value();
}

void NFTGalleryDelegate::paintHashArt(QPainter* painter, const QRect& rect,
                                      const QString& docHashHex) {
    // Derive 32 deterministic bytes from the fingerprint so the SAME NFT always
    // gets the SAME face (and two different NFTs almost never collide). A bare hex
    // anchor is already 32 bytes of entropy; a missing/odd hash is folded through
    // SHA-256 so we always have a full seed (and an empty hash still yields a
    // stable, calm tile rather than a crash).
    QByteArray seed = QByteArray::fromHex(docHashHex.toLatin1());
    if (seed.size() < 16)
        seed = QCryptographicHash::hash(docHashHex.toUtf8(), QCryptographicHash::Sha256);
    auto b = [&](int i) -> int {
        return seed.isEmpty() ? 0 : static_cast<uchar>(seed.at(i % seed.size()));
    };

    // Two stops on the dark palette: a hue from the hash, kept muted (mid sat /
    // value) so it never fights the card chrome or screams brighter than a real
    // photo would. The second stop is a darker shade of the same hue for depth.
    const int hue = b(0) * 360 / 256;
    QColor top  = QColor::fromHsv(hue, 90, 110);
    QColor bot  = QColor::fromHsv((hue + 28) % 360, 110, 64);
    QLinearGradient grad(rect.topLeft(), rect.bottomRight());
    grad.setColorAt(0.0, top);
    grad.setColorAt(1.0, bot);
    painter->fillRect(rect, grad);

    // A quiet 5x5 identicon, mirrored left<->right (the classic GitHub-style look),
    // in a soft light ink so it reads as "this card's mark", not noise. Cells are
    // chosen from the seed bits; the centre column + a small margin keep it tidy.
    const int grid = 5;
    const qreal cell = rect.width() / static_cast<qreal>(grid + 2);   // 1-cell margin each side
    const qreal ox = rect.left() + cell;
    const qreal oy = rect.top()  + (rect.height() - cell * grid) / 2.0;
    QColor ink("#e6e6e6"); ink.setAlpha(40);
    painter->setPen(Qt::NoPen);
    painter->setBrush(ink);
    const int cols = (grid + 1) / 2;     // 3 unique columns, mirrored to 5
    for (int gx = 0; gx < cols; ++gx) {
        for (int gy = 0; gy < grid; ++gy) {
            // one bit per cell from the seed stream
            const int bit = (b(1 + gx * grid + gy) >> (gy % 8)) & 1;
            if (!bit)
                continue;
            const QRectF c(ox + gx * cell, oy + gy * cell, cell, cell);
            painter->drawRect(c);
            const int mx = grid - 1 - gx;     // mirror across the centre column
            if (mx != gx) {
                const QRectF cm(ox + mx * cell, oy + gy * cell, cell, cell);
                painter->drawRect(cm);
            }
        }
    }
}

void NFTGalleryDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const {
    if (!index.isValid()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    // The card sits inside the item rect with a small margin so neighbours have a
    // visible gutter even without view spacing.
    const QRect cell = option.rect.adjusted(4, 4, -4, -4);

    const bool selected = (option.state & QStyle::State_Selected);
    const bool hovered  = (option.state & QStyle::State_MouseOver);

    // ---- card body ---------------------------------------------------------
    QPainterPath card;
    card.addRoundedRect(QRectF(cell), kCardRadius, kCardRadius);
    painter->fillPath(card, kCard);
    QColor border = kHair;
    if (selected)      border = kGreen;
    else if (hovered)  border = QColor("#3d4450");
    painter->setPen(QPen(border, selected ? 1.6 : 1.0));
    painter->drawPath(card);

    // ---- thumbnail area (square, top of the card) --------------------------
    const int thumbSize = cell.width() - 2 * kPad;
    const QRect thumbRect(cell.left() + kPad, cell.top() + kPad, thumbSize, thumbSize);
    QPainterPath thumbClip;
    thumbClip.addRoundedRect(QRectF(thumbRect), kThumbRadius, kThumbRadius);
    painter->fillPath(thumbClip, kInset);

    const int verifyState = index.data(NFTGalleryModel::VerifyStateRole).toInt();
    const QPixmap thumb = index.data(NFTGalleryModel::ThumbnailRole).value<QPixmap>();

    if (!thumb.isNull()) {
        painter->save();
        painter->setClipPath(thumbClip);
        // Cover-fit: scale keeping aspect, center, crop to the square.
        QPixmap scaled = thumb.scaled(thumbRect.size(), Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
        const int dx = (scaled.width()  - thumbRect.width())  / 2;
        const int dy = (scaled.height() - thumbRect.height()) / 2;
        painter->drawPixmap(thumbRect.topLeft(), scaled, QRect(dx, dy,
                            thumbRect.width(), thumbRect.height()));
        painter->restore();
    } else {
        // No local image bytes (we never auto-fetch — privacy hard rule). Instead of a
        // cold "Image not on this device" slab that reads as broken/empty, paint a
        // FRIENDLY, deterministic hash-art tile derived from the on-chain fingerprint:
        // a unique 2-stop gradient + a quiet identicon grid, so every card has its own
        // recognisable, never-blank face. A small bottom affordance ("tap to add image")
        // states the honest next step WITHOUT implying a download we never perform.
        const QString docHash = index.data(NFTGalleryModel::DocHashRole).toString();
        painter->save();
        painter->setClipPath(thumbClip);
        paintHashArt(painter, thumbRect, docHash);

        // A subtle bottom scrim + one-line affordance. Honest: no "download" verb — the
        // image lives on the owner's computer; opening the card lets them point at it.
        QRect strip(thumbRect.left(), thumbRect.bottom() - 22,
                    thumbRect.width(), 22);
        QColor scrim("#0f1115"); scrim.setAlpha(150);
        painter->fillRect(strip, scrim);
        QFont capFont = option.font;
        if (capFont.pointSizeF() > 0) capFont.setPointSizeF(capFont.pointSizeF() * 0.74);
        painter->setFont(capFont);
        painter->setPen(QColor("#c8ccd4"));
        const QFontMetrics cfmCap(capFont);
        painter->drawText(strip, Qt::AlignCenter,
                          cfmCap.elidedText(tr("Tap to add image"),
                                            Qt::ElideRight, strip.width() - 8));
        painter->restore();
    }

    // ---- verify badge (top-right corner of the thumbnail) ------------------
    // When there is NO local image, make NO verify claim — show a neutral dash badge
    // (matches the detail dialog's "can't check" terminal state) rather than the amber
    // "?" pending glyph, which falsely reads as "still verifying" forever (review fix #4).
    const QPixmap& vIcon = tintedIcon(verifyIconResource(verifyState),
                                      verifyColor(verifyState), kVerifyPx);
    const int badgePad = 6;
    const QRect badgeBg(thumbRect.right() - kVerifyPx - 2 * 4,
                        thumbRect.top() + badgePad - 4,
                        kVerifyPx + 2 * 4, kVerifyPx + 2 * 4);
    // a small dark disc behind the badge so it reads over any thumbnail.
    painter->setPen(Qt::NoPen);
    QColor disc("#0f1115"); disc.setAlpha(200);
    painter->setBrush(disc);
    painter->drawEllipse(badgeBg);
    if (thumb.isNull()) {
        // Neutral terminal "no claim" badge.
        QFont dashFont = option.font;
        dashFont.setBold(true);
        painter->setFont(dashFont);
        painter->setPen(kDim);
        painter->drawText(badgeBg, Qt::AlignCenter, QStringLiteral("–"));
    } else if (!vIcon.isNull()) {
        painter->drawPixmap(badgeBg.left() + 4, badgeBg.top() + 4, vIcon);
    }

    // ---- ownership pill (below the thumbnail) ------------------------------
    // #119 HONESTY: ZSLP NFT ownership is ALWAYS public (the token rides a public
    // transparent dust UTXO). NEVER render a green "Private" pill that would imply
    // shielded ownership — always the neutral/amber "Public" pill, regardless of the
    // (now-defunct) IsPrivateRole. The pill states the true settlement, nothing more.
    const QString pLabel = QStringLiteral("Public");
    const QColor  pCol   = kAmber;

    QFont pillFont = option.font;
    if (pillFont.pointSizeF() > 0) pillFont.setPointSizeF(pillFont.pointSizeF() * 0.78);
    pillFont.setBold(true);
    const QFontMetrics pfm(pillFont);
    const int pillTextW = pfm.horizontalAdvance(pLabel);
    const int pillW = pillTextW + 16;
    const int pillY = thumbRect.bottom() + kPad;
    const QRect pill(cell.left() + kPad, pillY, pillW, kPillH);

    QColor pillFill = pCol; pillFill.setAlpha(38);
    painter->setFont(pillFont);
    painter->setPen(QPen(pCol, 1.0));
    painter->setBrush(pillFill);
    painter->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5),
                             kPillH / 2.0, kPillH / 2.0);
    painter->setPen(pCol);
    painter->drawText(pill, Qt::AlignCenter, pLabel);

    // ---- name + collection caption -----------------------------------------
    const QString name       = index.data(NFTGalleryModel::NameRole).toString();
    const QString collection = index.data(NFTGalleryModel::CollectionRole).toString();

    int textTop = pill.bottom() + 6;

    QFont nameFont = option.font;
    nameFont.setBold(true);
    painter->setFont(nameFont);
    painter->setPen(selected ? option.palette.color(QPalette::HighlightedText) : kText);
    const QFontMetrics nfm(nameFont);
    const QRect nameRect(cell.left() + kPad, textTop,
                         cell.width() - 2 * kPad, nfm.height());
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      nfm.elidedText(name, Qt::ElideRight, nameRect.width()));

    QFont collFont = option.font;
    if (collFont.pointSizeF() > 0) collFont.setPointSizeF(collFont.pointSizeF() * 0.85);
    painter->setFont(collFont);
    painter->setPen(kDim);
    const QFontMetrics cfm(collFont);
    const QRect collRect(cell.left() + kPad, nameRect.bottom() + 2,
                         cell.width() - 2 * kPad, cfm.height());
    painter->drawText(collRect, Qt::AlignLeft | Qt::AlignVCenter,
                      cfm.elidedText(collection, Qt::ElideRight, collRect.width()));

    painter->restore();
}

QSize NFTGalleryDelegate::sizeHint(const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    // Fixed device-independent card, scaled by the device pixel ratio so the
    // layout reserves crisp space on HiDPI. (QListView::setUniformItemSizes is on,
    // so this is queried once and reused for every item.)
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const QSize base = baseCardSize();
    return QSize(qRound(base.width() * dpr), qRound(base.height() * dpr));
}

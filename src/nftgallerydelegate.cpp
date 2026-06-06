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

QString NFTGalleryDelegate::privacyLabel(bool isPrivate) {
    return isPrivate ? QStringLiteral("Private") : QStringLiteral("Public");
}

QColor NFTGalleryDelegate::privacyColor(bool isPrivate) {
    return isPrivate ? kGreen : kAmber;
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
        // Shimmer placeholder while the thumbnail is pending: a soft diagonal
        // sweep over the inset. Static (no animation timer in the shell), but
        // visually communicates "loading" vs an empty card.
        painter->save();
        painter->setClipPath(thumbClip);
        QLinearGradient g(thumbRect.topLeft(), thumbRect.bottomRight());
        g.setColorAt(0.0, kInset);
        g.setColorAt(0.5, QColor("#262a33"));
        g.setColorAt(1.0, kInset);
        painter->fillRect(thumbRect, g);
        painter->restore();
    }

    // ---- verify badge (top-right corner of the thumbnail) ------------------
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
    if (!vIcon.isNull())
        painter->drawPixmap(badgeBg.left() + 4, badgeBg.top() + 4, vIcon);

    // ---- privacy pill (below the thumbnail) --------------------------------
    const bool isPrivate = index.data(NFTGalleryModel::IsPrivateRole).toBool();
    const QString pLabel = privacyLabel(isPrivate);
    const QColor  pCol   = privacyColor(isPrivate);

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

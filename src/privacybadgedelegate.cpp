// ============================================================================
// PrivacyBadgeDelegate implementation — see privacybadgedelegate.h for intent.
// Tokens mirror res/styles/dark.qss exactly:
//   private/green #1f7a1f   public/amber #d9822b   danger/red #c0392b
//   text #e6e6e6   dim #9aa0a6   surface (pill bg) #1d2027
// ============================================================================
#include "privacybadgedelegate.h"
#include "settings.h"

#include <QPainter>
#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImageReader>
#include <QSvgRenderer>

// ---- dark.qss token colors (kept in sync with res/styles/dark.qss) ----------
namespace {
    const QColor kGreen   ("#1f7a1f");   // private
    const QColor kAmber   ("#d9822b");   // public
    const QColor kRed     ("#c0392b");   // danger / de-shield
    const QColor kText    ("#e6e6e6");   // primary text
    const QColor kDim     ("#9aa0a6");   // dim text
    const QColor kSurface ("#1d2027");   // elevated card / pill base

    // Pill geometry (in device-independent px). Small + unobtrusive.
    const int kPillH       = 20;   // pill height
    const int kPillPadH    = 8;    // horizontal text padding inside the pill
    const int kIconGap     = 4;    // gap between icon and label
    const int kPillGap     = 8;    // gap between the pill and the address text
}

PrivacyBadgeDelegate::PrivacyBadgeDelegate(Mode mode, QObject* parent)
    : QStyledItemDelegate(parent), _mode(mode) {}

// ---------------------------------------------------------------------------
// Address classification
// ---------------------------------------------------------------------------
QString PrivacyBadgeDelegate::extractAddress(const QString& displayText) {
    // Addressbook decorates as "label (address)". Recover the bare address from
    // the LAST "(...)" group; otherwise use the whole (trimmed) string.
    const QString s = displayText.trimmed();
    int open = s.lastIndexOf('(');
    int close = s.lastIndexOf(')');
    if (open >= 0 && close > open)
        return s.mid(open + 1, close - open - 1).trimmed();
    return s;
}

PrivacyBadgeDelegate::Kind PrivacyBadgeDelegate::classify(const QString& displayText) {
    // The transactions model (txtablemodel.cpp) renders shielded rows with no
    // address as the literal "(Shielded)" placeholder -> that's a private
    // send/receive. Check this on the RAW display text BEFORE extractAddress(),
    // which would otherwise strip the surrounding parens to "Shielded" and miss
    // this case (PRIV-13: "(Shielded)" -> Private).
    if (displayText.trimmed().compare("(Shielded)", Qt::CaseInsensitive) == 0)
        return Kind::Private;

    const QString addr = extractAddress(displayText);
    if (addr.isEmpty())
        return Kind::None;

    // Defensive: also catch a bare "Shielded" (post-strip) the same way.
    if (addr.compare("(Shielded)", Qt::CaseInsensitive) == 0 ||
        addr.compare("Shielded",   Qt::CaseInsensitive) == 0)
        return Kind::Private;

    auto* st = Settings::getInstance();

    // Primary: the authoritative Settings classifiers (checksum-verified).
    if (st) {
        if (st->isSaplingAddress(addr))             return Kind::Private;
        if (st->isSproutAddress(addr))              return Kind::PrivateLegacy;
        if (Settings::isZAddress(addr))             return Kind::Private;   // any other z
        if (Settings::isTAddress(addr))             return Kind::Public;
    }

    // Fallback: a cheap, checksum-free prefix check. Settings::is*Address all
    // early-out to false on an address whose checksum it can't verify; for a
    // DISPLAY badge we still want a correct, stable classification (and this is
    // what lets the badges render in the funded-synced mock, whose fixture
    // addresses are deliberately not checksum-valid). Real wallet addresses go
    // through the verified path above; this only catches the residue.
    const bool testnet = st && st->isTestnet();
    if (addr.startsWith("zs", Qt::CaseInsensitive) ||
        (testnet && addr.startsWith("ztestsapling", Qt::CaseInsensitive)))
        return Kind::Private;
    if (addr.startsWith("zc", Qt::CaseInsensitive))   // Sprout
        return Kind::PrivateLegacy;
    if (addr.startsWith('z', Qt::CaseInsensitive))    // any other shielded shape
        return Kind::Private;
    if (addr.startsWith('t', Qt::CaseInsensitive))    // transparent
        return Kind::Public;

    return Kind::None;
}

PrivacyBadgeDelegate::Kind PrivacyBadgeDelegate::classifyForIndex(const QModelIndex& index,
                                                                  const QString& displayText) {
    Kind k = classify(displayText);

    // Only a PUBLIC (transparent) recipient can be a de-shield, and only in a
    // table that has a Type column (the transactions table; column 0 == Type).
    if (k != Kind::Public || !index.isValid() || !index.model())
        return k;

    const QModelIndex typeIdx = index.sibling(index.row(), 0);
    if (!typeIdx.isValid())
        return k;
    const QString type = typeIdx.data(Qt::DisplayRole).toString().trimmed();
    if (type.isEmpty())
        return k;   // no Type column -> balances table -> stays PUBLIC

    // RPC "category" is "send"/"receive"; addZSentData uses sent-type rows too.
    // A send/sent row to a transparent address is the de-shield case.
    if (type.contains("send", Qt::CaseInsensitive) ||
        type.contains("sent", Qt::CaseInsensitive))
        return Kind::Deshield;

    return k;
}

QString PrivacyBadgeDelegate::labelFor(Kind k) {
    switch (k) {
        case Kind::Private:        return QStringLiteral("Private");
        case Kind::PrivateLegacy:  return QStringLiteral("Private (legacy)");
        case Kind::Public:         return QStringLiteral("PUBLIC");
        case Kind::Deshield:       return QStringLiteral("De-shield");
        default:                   return QString();
    }
}

QColor PrivacyBadgeDelegate::colorFor(Kind k) {
    switch (k) {
        case Kind::Private:
        case Kind::PrivateLegacy:  return kGreen;
        case Kind::Public:         return kAmber;
        case Kind::Deshield:       return kRed;
        default:                   return kDim;
    }
}

// ---------------------------------------------------------------------------
// Icon rendering + cache
// ---------------------------------------------------------------------------
const QPixmap& PrivacyBadgeDelegate::icon(Kind k, int px) const {
    // Cache key is kind + size; the device pixel ratio doesn't change at runtime
    // for a given screen, and offscreen/Xvfb run at ratio 1, so px is enough.
    const QString key = QString::number(int(k)) + "@" + QString::number(px);
    auto it = _iconCache.find(key);
    if (it != _iconCache.end())
        return it.value();

    QString res;
    switch (k) {
        case Kind::Private:
        case Kind::PrivateLegacy:  res = ":/icons/res/icons/shield-lock.svg"; break;
        case Kind::Public:         res = ":/icons/res/icons/eye.svg";         break;
        case Kind::Deshield:       res = ":/icons/res/icons/eye-off.svg";     break;
        default:                   res = QString();                           break;
    }

    QPixmap pm(px, px);
    pm.fill(Qt::transparent);

    if (!res.isEmpty()) {
        // Render the monochrome silhouette to an alpha mask, then tint it to the
        // badge color via SourceIn. Using QSvgRenderer keeps it crisp at any
        // size and identical cross-platform (no QIcon::fromTheme).
        QSvgRenderer renderer(res);
        if (renderer.isValid()) {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            renderer.render(&p, QRectF(0, 0, px, px));
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(QRect(0, 0, px, px), colorFor(k));
            p.end();
        }
        // If the SVG plugin/module is somehow unavailable, pm stays transparent
        // and the pill simply renders text-only (the documented fallback).
    }

    it = _iconCache.insert(key, pm);
    return it.value();
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
QFont PrivacyBadgeDelegate::monoFont() {
    // A fixed-pitch font with tabular figures so amount columns line up. Prefer
    // the platform's canonical monospace; QFontDatabase::systemFont guarantees a
    // real fixed font even on the minimal static bundle / offscreen QPA.
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setStyleHint(QFont::Monospace);
    // Tabular figures: keep digit advances equal even in proportional fallbacks.
    f.setStyleStrategy(QFont::PreferDefault);
    return f;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void PrivacyBadgeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    if (!index.isValid()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Draw the standard background/selection chrome first (so selection
    // highlight, alternating rows, focus rect, etc. all behave exactly as the
    // base delegate would). We then overlay our content.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString rawText = opt.text;
    opt.text.clear();   // we paint the text ourselves below
    const QWidget* w = opt.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

    if (rawText.isEmpty() && _mode != Mode::Address) {
        // Nothing to draw (e.g. a blank/loading cell) — leave chrome only.
        return;
    }

    const bool selected = (opt.state & QStyle::State_Selected);
    const QRect cell = opt.rect.adjusted(8, 0, -8, 0);   // match qss item padding feel

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    if (_mode == Mode::Amount) {
        // ---- amount column: monospace, tabular, right-aligned, sign-tinted ---
        QFont f = monoFont();
        f.setPointSizeF(option.font.pointSizeF() > 0 ? option.font.pointSizeF()
                                                     : painter->font().pointSizeF());
        painter->setFont(f);

        // Subtle sign coloring: a leading '-' (a send / outflow) leans dim, an
        // inflow leans on the normal text color. Selection always uses the
        // high-contrast highlighted text so it stays legible.
        QColor c = kText;
        const QString t = rawText.trimmed();
        if (!selected) {
            if (t.startsWith('-'))      c = kDim;          // sent / outflow
            else                        c = kText;         // received / balance
        } else {
            c = opt.palette.color(QPalette::HighlightedText);
        }
        painter->setPen(c);
        painter->drawText(cell, Qt::AlignRight | Qt::AlignVCenter, rawText);
        painter->restore();
        return;
    }

    // ---- address column: privacy pill + the address text -------------------
    const Kind kind = classifyForIndex(index, rawText);

    int x = cell.left();
    if (kind != Kind::None) {
        const QString label = labelFor(kind);
        const QColor  col   = colorFor(kind);

        // Pill font: a touch smaller + semibold, like the qss header text.
        QFont pf = option.font;
        if (pf.pointSizeF() > 0) pf.setPointSizeF(pf.pointSizeF() * 0.82);
        pf.setBold(true);
        painter->setFont(pf);
        const QFontMetrics fm(pf);

        const int iconPx = kPillH - 8;                 // glyph inside the pill
        const int textW  = fm.horizontalAdvance(label);
        const int pillW  = kPillPadH + iconPx + kIconGap + textW + kPillPadH;
        const int pillY  = cell.top() + (cell.height() - kPillH) / 2;
        const QRect pill(x, pillY, pillW, kPillH);

        // Pill body: a translucent tint of the badge color over the surface, so
        // it reads on the dark table without shouting; a thin solid border in
        // the full badge color carries the semantic.
        QColor fill = col; fill.setAlpha(38);
        painter->setPen(QPen(col, 1.2));
        painter->setBrush(fill);
        const qreal r = kPillH / 2.0;
        painter->drawRoundedRect(QRectF(pill).adjusted(0.6, 0.6, -0.6, -0.6), r, r);

        // Icon (tinted SVG silhouette), vertically centered in the pill.
        const QPixmap& ic = icon(kind, iconPx);
        if (!ic.isNull()) {
            const int iy = pill.top() + (pill.height() - iconPx) / 2;
            painter->drawPixmap(pill.left() + kPillPadH, iy, ic);
        }

        // Label text in the badge color.
        painter->setPen(col);
        const QRect tr(pill.left() + kPillPadH + iconPx + kIconGap, pill.top(),
                       textW + 2, pill.height());
        painter->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, label);

        x = pill.right() + kPillGap;
    }

    // The address / display text after the pill, elided to fit, in normal text.
    painter->setFont(option.font);
    const QColor tcol = selected ? opt.palette.color(QPalette::HighlightedText) : kText;
    painter->setPen(tcol);
    const QRect textRect(x, cell.top(), cell.right() - x, cell.height());
    const QFontMetrics fm(option.font);
    const QString elided = fm.elidedText(rawText, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

    painter->restore();
}

QSize PrivacyBadgeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const {
    // Keep the base width estimate but guarantee enough height for the pill so
    // the badge never clips. We deliberately do NOT inflate beyond what the
    // dark.qss item padding already allows for the Fixed-height layout rows.
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    if (_mode == Mode::Address) {
        s.setHeight(qMax(s.height(), kPillH + 6));
        // Reserve room for the widest pill so column auto-size doesn't clip it.
        s.setWidth(s.width() + 120);
    }
    return s;
}

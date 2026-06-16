#ifndef GUIUTIL_H
#define GUIUTIL_H

// Small shared GUI helpers, header-only so any window/dialog can use them:
//   (1) makeLabelsSelectable — so ALL text (errors, balances, ids, fields) is copy-pasteable.
//   (2) makeButtonsFit       — so no button ever clips its label at the user's font / DPI.
// Both only ever ADD capability / GROW a minimum; they never remove flags or shrink a widget.

#include <QWidget>
#include <QLabel>
#include <QAbstractButton>
#include <QList>

// Make every QLabel under `root` user-selectable (preserving existing flags such as clickable
// links). Idempotent. Selectability is independent of font/DPI, so calling it from a ctor is fine.
inline void makeLabelsSelectable(QWidget* root) {
    if (!root) return;
    const QList<QLabel*> labels = root->findChildren<QLabel*>();
    for (QLabel* l : labels) {
        if (l)
            l->setTextInteractionFlags(l->textInteractionFlags() | Qt::TextSelectableByMouse);
    }
}

// Ensure every button under `root` is at least as wide/tall as its label needs at the CURRENT
// (post-style, post-DPI) font, so the text is never clipped. Only grows a button's minimum and
// lifts a too-small maximum; never shrinks. Call AFTER the widget is polished (e.g. in showEvent)
// so sizeHint() reflects the real rendered font.
inline void makeButtonsFit(QWidget* root) {
    if (!root) return;
    const QList<QAbstractButton*> btns = root->findChildren<QAbstractButton*>();
    for (QAbstractButton* b : btns) {
        if (!b) continue;
        const QSize hint = b->sizeHint();
        if (hint.width() > b->minimumWidth())
            b->setMinimumWidth(hint.width());
        if (b->maximumWidth() < hint.width())
            b->setMaximumWidth(QWIDGETSIZE_MAX);          // never let a stale cap clip the label
        if (hint.height() > b->minimumHeight())
            b->setMinimumHeight(hint.height());
    }
}

#endif // GUIUTIL_H

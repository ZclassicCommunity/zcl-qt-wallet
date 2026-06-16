// Copyright 2019-2024 The Hush developers / ZClassic contributors
// Released under the GPLv3
//
// A small, standard "flow" layout: lays its items out left-to-right and wraps to
// the next line when it runs out of horizontal room (like word-wrap for widgets).
// This is the canonical Qt FlowLayout example, trimmed to what the Collections
// heading row needs, so the 4 NFT action buttons re-flow instead of clipping on a
// narrow window. No Q_OBJECT (plain QLayout subclass) -> no MOC required.
#ifndef FLOWLAYOUT_H
#define FLOWLAYOUT_H

#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>
#include <QStyle>

class QWidget;
class QLayoutItem;

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
    explicit FlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QLayoutItem* takeAt(int index) override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem*> itemList;
    int m_hSpace;
    int m_vSpace;
};

#endif // FLOWLAYOUT_H

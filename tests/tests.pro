# ============================================================================
# tst_logic — L0 unit test target for the ZClassic Qt5 GUI wallet.
#
# Links ONLY pure-logic product TUs (settings.cpp, senttxstore.cpp,
# addresscombo.cpp) + test shims. No daemon, no libsodium, no main.cpp.
#
# Build + run (inside the proot chroot):
#   /home/rhett/zclbuild/prun bash -c '\
#     cd /src/wallet/tests && /opt/qt-static/bin/qmake tests.pro && make -j$(nproc) && \
#     QT_QPA_PLATFORM=offscreen ./tst_logic'
# ============================================================================
TEMPLATE = app
TARGET   = tst_logic

CONFIG  += c++14 console
CONFIG  -= app_bundle

# widgets is required only because addresscombo.cpp derives from QComboBox.
# We run guiless via QT_QPA_PLATFORM=offscreen (no X server needed).
QT      += core testlib widgets network gui

# Keep all build artifacts inside tests/bin so they NEVER collide with the
# app's own bin/ (which holds the product objects/moc).
OBJECTS_DIR = bin
MOC_DIR     = bin
RCC_DIR     = bin
UI_DIR      = bin
DESTDIR     = .

# INCLUDEPATH ORDER IS LOad-BEARING: shim/ first so `#include "mainwindow.h"`,
# "rpc.h", "precompiled.h", "addressbook.h" resolve to the SHIMS, then ../src
# for the real settings.h / senttxstore.h / addresscombo.h, then 3rdparty.
INCLUDEPATH += $$PWD/shim
INCLUDEPATH += $$PWD/../src
INCLUDEPATH += $$PWD/../src/3rdparty

# addresscombo.h carries Q_OBJECT -> qmake must run moc on it (generates the
# vtable). Listing it in HEADERS triggers that without touching the source file.
HEADERS += ../src/addresscombo.h

# Product pure-logic sources (UNMODIFIED) + shims + the test.
SOURCES += \
    ../src/settings.cpp \
    ../src/senttxstore.cpp \
    ../src/addresscombo.cpp \
    shim/addressbook_stub.cpp \
    tst_logic.cpp

# Static-Qt offscreen platform plugin import (no X server in the chroot).
static {
    QTPLUGIN.platforms = qoffscreen
}

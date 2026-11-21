# The offscreen harness: bin/harness builds it into build/harness.
QT += core gui qml quick quickcontrols2 dbus testlib
CONFIG += c++17
TARGET = harness
TEMPLATE = app

SRC = ../../src
INCLUDEPATH += $$SRC
HEADERS += $$SRC/accountlist.h $$SRC/accountmove.h $$SRC/accounts.h $$SRC/backend.h $$SRC/blockeddeletes.h $$SRC/clockformat.h $$SRC/conflictcache.h $$SRC/controlcommands.h $$SRC/fixcommand.h $$SRC/fixstate.h \
           $$SRC/listingdiff.h $$SRC/machineguard.h $$SRC/notifier.h $$SRC/takeover.h $$SRC/firstsetup.h $$SRC/statuscommand.h $$SRC/settingscommand.h \
           $$SRC/pausestate.h $$SRC/pluginsetup.h $$SRC/snapshot.h $$SRC/syncconfig.h $$SRC/synclog.h \
           $$SRC/setupcontroller.h $$SRC/settingscontroller.h $$SRC/foldertree.h $$SRC/selectivesync.h $$SRC/filterrules.h $$SRC/filtereditor.h $$SRC/localtree.h $$SRC/takeoversetup.h $$SRC/theme.h $$SRC/translator.h
SOURCES += harness.cpp $$SRC/accountlist.cpp $$SRC/accountmove.cpp $$SRC/accounts.cpp $$SRC/backend.cpp $$SRC/blockeddeletes.cpp $$SRC/clockformat.cpp $$SRC/conflictcache.cpp $$SRC/controlcommands.cpp $$SRC/fixcommand.cpp $$SRC/fixstate.cpp \
           $$SRC/listingdiff.cpp $$SRC/machineguard.cpp $$SRC/notifier.cpp $$SRC/takeover.cpp $$SRC/firstsetup.cpp $$SRC/statuscommand.cpp $$SRC/settingscommand.cpp \
           $$SRC/pausestate.cpp $$SRC/pluginsetup.cpp $$SRC/snapshot.cpp $$SRC/syncconfig.cpp $$SRC/synclog.cpp \
           $$SRC/setupcontroller.cpp $$SRC/settingscontroller.cpp $$SRC/foldertree.cpp $$SRC/selectivesync.cpp $$SRC/filterrules.cpp $$SRC/filtereditor.cpp $$SRC/localtree.cpp $$SRC/takeoversetup.cpp $$SRC/theme.cpp $$SRC/translator.cpp
RESOURCES += $$SRC/resources.qrc

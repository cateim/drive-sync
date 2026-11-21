QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = drive-sync
TEMPLATE = app

HEADERS += \
    src/accountmove.h \
    src/accountlist.h \
    src/accounts.h \
    src/backend.h \
    src/blockeddeletes.h \
    src/clockformat.h \
    src/conflictcache.h \
    src/controlcommands.h \
    src/firstsetup.h \
    src/fixcommand.h \
    src/fixstate.h \
    src/legacymigration.h \
    src/listingdiff.h \
    src/machineguard.h \
    src/notifier.h \
    src/oauthclient.h \
    src/pausestate.h \
    src/pluginsetup.h \
    src/selectivesync.h \
    src/filterrules.h \
    src/filtereditor.h \
    src/localtree.h \
    src/settingscommand.h \
    src/settingscontroller.h \
    src/foldertree.h \
    src/setupcontroller.h \
    src/signin.h \
    src/snapshot.h \
    src/statuscommand.h \
    src/syncconfig.h \
    src/synclog.h \
    src/takeover.h \
    src/takeoversetup.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/accountmove.cpp \
    src/accountlist.cpp \
    src/accounts.cpp \
    src/backend.cpp \
    src/blockeddeletes.cpp \
    src/clockformat.cpp \
    src/conflictcache.cpp \
    src/controlcommands.cpp \
    src/firstsetup.cpp \
    src/fixcommand.cpp \
    src/fixstate.cpp \
    src/legacymigration.cpp \
    src/listingdiff.cpp \
    src/machineguard.cpp \
    src/notifier.cpp \
    src/oauthclient.cpp \
    src/pausestate.cpp \
    src/pluginsetup.cpp \
    src/selectivesync.cpp \
    src/filterrules.cpp \
    src/filtereditor.cpp \
    src/localtree.cpp \
    src/settingscommand.cpp \
    src/settingscontroller.cpp \
    src/foldertree.cpp \
    src/setupcontroller.cpp \
    src/signin.cpp \
    src/snapshot.cpp \
    src/statuscommand.cpp \
    src/syncconfig.cpp \
    src/synclog.cpp \
    src/takeover.cpp \
    src/takeoversetup.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc src/signin-page.qrc

# The Google OAuth client is compiled in only from the git-ignored
# src/oauthclient.local.h (see src/oauthclient.local.h.example); without it
# the build has no client and first setup says so.
exists($$PWD/src/oauthclient.local.h): DEFINES += HAVE_LOCAL_OAUTH

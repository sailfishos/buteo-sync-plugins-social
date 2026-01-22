CONFIG += link_pkgconfig

PKGCONFIG += \
    libsailfishkeyprovider \
    libsignon-qt5 \
    accounts-qt5 \
    buteosyncfw5 \
    socialcache

QT += \
    network \
    dbus \
    sql

QT -= gui

QMAKE_CXXFLAGS += -Wsuggest-override
QMAKE_LFLAGS += $$QMAKE_LFLAGS_NOUNDEF

DEFINES += 'SYNC_DATABASE_DIR=\'\"Sync\"\''

INCLUDEPATH += . $$PWD/common/

LIBS += -L$$PWD/common -lsyncpluginscommon

contains(DEFINES, 'SOCIALD_USE_QTPIM') {
    DEFINES *= USE_CONTACTS_NAMESPACE=QTCONTACTS_USE_NAMESPACE
    PKGCONFIG += Qt5Contacts qtcontacts-sqlite-qt5-extensions
    HEADERS += $$PWD/common/constants_p.h

    # We need the moc output for ContactManagerEngine from sqlite-extensions
    extensionsIncludePath = $$system(pkg-config --cflags-only-I qtcontacts-sqlite-qt5-extensions)
    VPATH += $$replace(extensionsIncludePath, -I, )
    HEADERS += contactmanagerengine.h
}

TEMPLATE = lib
CONFIG += plugin
target.path = $$[QT_INSTALL_LIBS]/buteo-plugins-qt5/oopp

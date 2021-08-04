TARGET = knowncontacts-client

include($$PWD/../common.pri)

knowncontacts_sync_profile.path = /etc/buteo/profiles/sync
knowncontacts_sync_profile.files = knowncontacts.Contacts.xml
knowncontacts_client_plugin_xml.path = /etc/buteo/profiles/client
knowncontacts_client_plugin_xml.files = knowncontacts.xml

HEADERS += \
    knowncontactsplugin.h \
    knowncontactssyncer.h

SOURCES += \
    knowncontactsplugin.cpp \
    knowncontactssyncer.cpp

OTHER_FILES = \
    knowncontacts.Contacts.xml
    knowncontacts.xml

QT -= gui
QT += dbus

TEMPLATE = lib
CONFIG += plugin link_pkgconfig

PKGCONFIG += buteosyncfw5 Qt5Contacts qtcontacts-sqlite-qt5-extensions

QMAKE_CXXFLAGS = -Wall \
    -g \
    -Wno-cast-align \
    -O2 -finline-functions

target.path = $$[QT_INSTALL_LIBS]/buteo-plugins-qt5/oopp

INSTALLS += target \
    knowncontacts_sync_profile \
    knowncontacts_client_plugin_xml

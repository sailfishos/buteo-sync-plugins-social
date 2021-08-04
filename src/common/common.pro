TEMPLATE = lib

QT -= gui
QT += network dbus

CONFIG += link_pkgconfig
PKGCONFIG += \
    accounts-qt5 \
    buteosyncfw5 \
    socialcache \

TARGET = syncpluginscommon
TARGET = $$qtLibraryTarget($$TARGET)

HEADERS += \
    $$PWD/buteosyncfw_p.h \
    $$PWD/socialdbuteoplugin.h \
    $$PWD/socialnetworksyncadaptor.h \
    $$PWD/socialdnetworkaccessmanager_p.h \
    $$PWD/trace.h

SOURCES += \
    $$PWD/socialdbuteoplugin.cpp \
    $$PWD/socialnetworksyncadaptor.cpp \
    $$PWD/socialdnetworkaccessmanager_p.cpp \
    $$PWD/trace.cpp

TARGETPATH = $$[QT_INSTALL_LIBS]
target.path = $$TARGETPATH

INSTALLS += target

TEMPLATE = lib

QT -= gui
QT += network dbus

isEmpty(PREFIX) {
    PREFIX=/usr
}

CONFIG += link_pkgconfig
PKGCONFIG += \
    accounts-qt5 \
    buteosyncfw5 \
    socialcache \

TARGET = buteosocialcommon
TARGET = $$qtLibraryTarget($$TARGET)

HEADERS += \
    $$PWD/buteosyncfw_p.h \
    $$PWD/constants_p.h \
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
common_headers.path = $$PREFIX/include/buteosocialcommon
common_headers.files = \
    $$PWD/socialdbuteoplugin.h \
    $$PWD/socialnetworksyncadaptor.h
common_pkgconfig.path = $$[QT_INSTALL_LIBS]/pkgconfig
common_pkgconfig.files = $$PWD/buteosocialcommon.pc

INSTALLS += target common_headers common_pkgconfig

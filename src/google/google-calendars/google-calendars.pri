CONFIG += link_pkgconfig
PKGCONFIG += libmkcal-qt5 KF5CalendarCore

SOURCES += \
    $$PWD/googlecalendarsyncadaptor.cpp

HEADERS += \
    $$PWD/googlecalendarsyncadaptor.h \
    $$PWD/googlecalendarincidencecomparator.h

INCLUDEPATH += $$PWD

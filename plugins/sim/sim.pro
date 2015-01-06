TEMPLATE = lib
QT -= gui
QT += dbus

CONFIG += plugin

CONFIG += link_pkgconfig
PKGCONFIG += mlite5 Qt5Contacts Qt5Versit qofono-qt5
PKGCONFIG += qtcontacts-sqlite-qt5-extensions

DEFINES += QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_ASCII

INCLUDEPATH += $$TOP_SOURCEDIR/src
DEFINES += ENABLE_DEBUG

HEADERS  = \
    cdsimcontroller.h \
    cdsimplugin.h

SOURCES  = \
    cdsimcontroller.cpp \
    cdsimplugin.cpp

TARGET = simplugin
target.path = $$LIBDIR/contactsd-1.0/plugins

INSTALLS += target

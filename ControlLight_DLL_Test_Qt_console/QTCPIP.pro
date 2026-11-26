#-------------------------------------------------
#
# Project created by QtCreator 2017-10-01T20:47:40
#
#-------------------------------------------------

#QT       += core gui network
QT -= gui
CONFIG += console
CONFIG -= app_bundle

SOURCES += main.cpp

# Qt wrapper for ControlLight.dll, shared by the Qt examples. Its API specific part is generated from
# ControlLight/ControlAPI.h by ControlLight_Qt_wrapper/generate_qt_wrapper.py.
CONTROL_LIGHT_QT_WRAPPER = $$PWD/../ControlLight_Qt_wrapper
INCLUDEPATH += $$CONTROL_LIGHT_QT_WRAPPER
HEADERS += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI.h
SOURCES += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI.cpp
OTHER_FILES += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI_generated_members.inc \
    $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI_generated_methods.inc

TARGET = QTCPIP
TEMPLATE = app

#INCLUDEPATH += $$PWD
#LIBS += -L$$PWD/lib -lControlLightAPI

#DEFINES += USING_DLL

# If needed (to find the DLL at runtime)
# QMAKE_POST_LINK += $$quote(cmd /c copy /y $$shell_path($$PWD/ControlAPI.dll) $$shell_path($$OUT_PWD))


#greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

#TARGET = QTelnet
#TEMPLATE = app

#INCLUDEPATH += $$PWD/include
#LIBS += -L$$PWD/lib -lControlAPI


#SOURCES += main.cpp\
#   ControlAPI.cpp \
#    QTCPIP.cpp \
#    QCmdWidget.cpp \
#    QTCPIPControlTester.cpp \
#    TestSequence.cpp

#HEADERS  += \
#    ControlAPI.h \
#    QCmdWidget.h \
#    QTCPIP.h \
#    QTCPIPControlTester.h \
#    TestSequence.h \
#    main.h

#FORMS    += \
#    QTCPIPControlTester.ui

#RESOURCES += \
#    resources.qrc


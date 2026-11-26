#-------------------------------------------------
#
# Project created by QtCreator 2017-10-01T20:47:40
#
#-------------------------------------------------

QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = QTelnet
TEMPLATE = app


SOURCES += main.cpp\
    QTCPIP.cpp \
    QCmdWidget.cpp \
    QTCPIPControlTester.cpp \
    TestSequence.cpp

HEADERS  += \
    QCmdWidget.h \
    QTCPIP.h \
    QTCPIPControlTester.h \
    TestSequence.h \
    main.h

FORMS    += \
    QTCPIPControlTester.ui

RESOURCES += \
    resources.qrc

DISTFILES += \
    ControlHardwareConfig.json

# Qt wrapper for ControlLight.dll, shared by the Qt examples. Its API specific part is generated from
# ControlLight/ControlAPI.h by ControlLight_Qt_wrapper/generate_qt_wrapper.py.
CONTROL_LIGHT_QT_WRAPPER = $$PWD/../ControlLight_Qt_wrapper
INCLUDEPATH += $$CONTROL_LIGHT_QT_WRAPPER
HEADERS += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI.h
SOURCES += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI.cpp
OTHER_FILES += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI_generated_members.inc \
    $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI_generated_methods.inc

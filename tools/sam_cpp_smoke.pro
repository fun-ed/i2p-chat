TEMPLATE = app
TARGET = sam_cpp_smoke
CONFIG += console c++17
CONFIG -= app_bundle
QT += core gui widgets
QMAKE_CXXFLAGS += -Wall -Wextra

INCLUDEPATH += $$PWD/../src/backend
SOURCES += $$PWD/sam_cpp_smoke.cpp \
    $$PWD/../src/backend/I2PSamMessageAnalyser.cpp
HEADERS += $$PWD/../src/backend/I2PSamMessageAnalyser.h

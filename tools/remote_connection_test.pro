QT += core gui network xml widgets multimedia svg testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
macx: QMAKE_MACOSX_DEPLOYMENT_TARGET = 15.0
TEMPLATE = app
TARGET = remote_connection_test

ROOT = $$PWD/..
INCLUDEPATH += $$ROOT/src/backend $$ROOT/src/gui

SOURCES += \
    remote_connection_test.cpp \
    $$ROOT/src/backend/Core.cpp \
    $$ROOT/src/backend/User.cpp \
    $$ROOT/src/backend/I2PSamMessageAnalyser.cpp \
    $$ROOT/src/backend/DebugMessageManager.cpp \
    $$ROOT/src/backend/Protocol.cpp \
    $$ROOT/src/backend/PacketManager.cpp \
    $$ROOT/src/backend/FileTransferSend.cpp \
    $$ROOT/src/backend/FileTransferReceive.cpp \
    $$ROOT/src/backend/SoundManager.cpp \
    $$ROOT/src/backend/ConnectionManager.cpp \
    $$ROOT/src/backend/RouterConnection.cpp \
    $$ROOT/src/backend/SessionController.cpp \
    $$ROOT/src/backend/I2PStream.cpp \
    $$ROOT/src/backend/ChatmessageChanger.cpp \
    $$ROOT/src/backend/UserBlockManager.cpp \
    $$ROOT/src/backend/HttpOverStreamObject.cpp \
    $$ROOT/src/backend/UserManager.cpp \
    $$ROOT/src/backend/FileTransferManager.cpp \
    $$ROOT/src/backend/TextEmotionChanger.cpp \
    $$ROOT/src/backend/NetworkSettingsCompat.cpp \
    $$ROOT/src/backend/UnsentChatMessageStorage.cpp

SOURCES += \
    $$ROOT/src/gui/form_Main.cpp \
    $$ROOT/src/gui/form_chatwidget.cpp \
    $$ROOT/src/gui/form_DebugMessages.cpp \
    $$ROOT/src/gui/form_fileReceive.cpp \
    $$ROOT/src/gui/form_fileSend.cpp \
    $$ROOT/src/gui/form_newUser.cpp \
    $$ROOT/src/gui/form_rename.cpp \
    $$ROOT/src/gui/form_settingsgui.cpp \
    $$ROOT/src/gui/form_UserSearch.cpp \
    $$ROOT/src/gui/form_about.cpp \
    $$ROOT/src/gui/form_TopicSubscribe.cpp

HEADERS += \
    $$ROOT/src/gui/form_Main.h \
    $$ROOT/src/gui/form_chatwidget.h \
    $$ROOT/src/gui/form_DebugMessages.h \
    $$ROOT/src/gui/form_fileReceive.h \
    $$ROOT/src/gui/form_fileSend.h \
    $$ROOT/src/gui/form_newUser.h \
    $$ROOT/src/gui/form_rename.h \
    $$ROOT/src/gui/form_settingsgui.h \
    $$ROOT/src/gui/form_UserSearch.h \
    $$ROOT/src/gui/form_about.h \
    $$ROOT/src/gui/gui_icons.h \
    $$ROOT/src/gui/form_TopicSubscribe.h

FORMS += \
    $$ROOT/src/gui/form_Main.ui \
    $$ROOT/src/gui/form_chatwidget.ui \
    $$ROOT/src/gui/form_DebugMessages.ui \
    $$ROOT/src/gui/form_fileReceive.ui \
    $$ROOT/src/gui/form_fileSend.ui \
    $$ROOT/src/gui/form_newUser.ui \
    $$ROOT/src/gui/form_rename.ui \
    $$ROOT/src/gui/form_settingsgui.ui \
    $$ROOT/src/gui/form_userSearch.ui \
    $$ROOT/src/gui/form_about.ui \
    $$ROOT/src/gui/form_topicSubscribe.ui

HEADERS += \
    $$ROOT/src/backend/Core.h \
    $$ROOT/src/backend/User.h \
    $$ROOT/src/backend/I2PSamMessageAnalyser.h \
    $$ROOT/src/backend/DebugMessageManager.h \
    $$ROOT/src/backend/Protocol.h \
    $$ROOT/src/backend/PacketManager.h \
    $$ROOT/src/backend/FileTransferSend.h \
    $$ROOT/src/backend/FileTransferReceive.h \
    $$ROOT/src/backend/SoundManager.h \
    $$ROOT/src/backend/ConnectionManager.h \
    $$ROOT/src/backend/RouterConnection.h \
    $$ROOT/src/backend/SessionController.h \
    $$ROOT/src/backend/I2PStream.h \
    $$ROOT/src/backend/ChatmessageChanger.h \
    $$ROOT/src/backend/UserBlockManager.h \
    $$ROOT/src/backend/HttpOverStreamObject.h \
    $$ROOT/src/backend/UserManager.h \
    $$ROOT/src/backend/FileTransferManager.h \
    $$ROOT/src/backend/TextEmotionChanger.h \
    $$ROOT/src/backend/NetworkSettingsCompat.h \
    $$ROOT/src/backend/UnsentChatMessageStorage.h

RESOURCES += $$ROOT/src/gui/resourcen.qrc

include($$ROOT/gitversion.pri)

# The console test target has no application bundle to update.
QMAKE_POST_LINK =

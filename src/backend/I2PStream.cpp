/***************************************************************************
 *   Copyright (C) 2008 by I2P-Messenger                                   *
 *   Messenger-Dev@I2P-Messenger                                           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "I2PStream.h"
#include "NetworkSettingsCompat.h"
#include <QNetworkProxy>
#include <QSettings>

const QString SAM_HANDSHAKE_V3 = "HELLO VERSION MIN=3.1 MAX=3.1\n";
const int CONNECTIONTIMEOUT = 60 * 1000;

namespace {

QString classifySocketErrorCode(const QAbstractSocket::SocketError error) {
  switch (error) {
  case QAbstractSocket::ConnectionRefusedError:
    return "Refused";
  case QAbstractSocket::SocketTimeoutError:
    return "Timeout";
  case QAbstractSocket::ProxyAuthenticationRequiredError:
    return "ProxyAuthFailed";
  case QAbstractSocket::ProxyConnectionRefusedError:
    return "ProxyRefused";
  case QAbstractSocket::ProxyConnectionClosedError:
    return "ProxyClosed";
  case QAbstractSocket::ProxyNotFoundError:
    return "ProxyNotFound";
  case QAbstractSocket::HostNotFoundError:
    return "HostNotFound";
  case QAbstractSocket::NetworkError:
    return "NetworkError";
  default:
    return "Failed";
  }
}


QByteArray buildHelloMessage() { return SAM_HANDSHAKE_V3.toUtf8(); }

QByteArray buildStreamCommand(const QString &streamBridgeName,
                              const QString &destination,
                              const StreamMode mode, const bool silence) {
  QByteArray message;
  if (mode == ACCEPT) {
    message.append(("STREAM ACCEPT ID=" + streamBridgeName).toUtf8());
  } else {
    message.append(("STREAM CONNECT ID=" + streamBridgeName + " DESTINATION=" +
                    destination)
                       .toUtf8());
  }

  if (silence == true) {
    message.append(" Silence=true\n");
  } else {
    message.append(" Silence=false\n");
  }
  return message;
}

} // namespace

CI2PStream::CI2PStream(QString mSamHost, QString mSamPort, qint32 mID,
                       QString mStreamBridgeName, StreamMode mMode,
                       bool mSilence, QString UsedFor, QString ConfigPath,
                       bool forceDirectProxy)
    : mSamHost(mSamHost), mSamPort(mSamPort), mID(mID),
      mStreamBridgeName(mStreamBridgeName), mMode(mMode),
      mConfigPath(ConfigPath), mForceDirectProxy(forceDirectProxy),
      mSilence(mSilence), mUsedFor(UsedFor) {
  mAnalyser = NULL;
  mIncomingPackets = NULL;
  mDoneDisconnect = false;
  mStatusReceived = false;
  mHandshakeSuccessful = false;
  mConnectionType = UNKNOWN;
  mIncomingPackets = new QByteArray();
  mDestinationReceived = false;
  mFIRSTPACKETCHAT_alreadySent = false;
  mCurrentRouteDescription = "Direct";
  mConnectTimeoutMs = 5000;
  mTimer = NULL;
  mUnKnownConnectionTimeout.setInterval(CONNECTIONTIMEOUT);

  connect(&mTcpSocket, SIGNAL(connected()), this, SLOT(slotConnected()));
  connect(&mTcpSocket, SIGNAL(disconnected()), this, SLOT(slotDisconnected()));
  connect(&mTcpSocket, SIGNAL(readyRead()), this, SLOT(slotReadFromSocket()));

  connect(&mUnKnownConnectionTimeout, SIGNAL(timeout()), this,
          SLOT(slotInitConnectionTimeout()));
}

CI2PStream::~CI2PStream() {
  if (mTimer != NULL) {
    delete mTimer;
  }

  disconnect(&mTcpSocket, SIGNAL(disconnected()), this,
             SLOT(slotDisconnected()));
  doDisconnect();

  mTcpSocket.deleteLater();

  if (mAnalyser != NULL) {
    delete mAnalyser;
  }

  if (mIncomingPackets != NULL) {
    delete mIncomingPackets;
  }
}

void CI2PStream::configureSocks5Proxy() {
  // 配置 SOCKS5 並同步更新連線路由資訊，方便除錯代理鏈路。
  if (mConfigPath.isEmpty()) {
    mTcpSocket.setProxy(QNetworkProxy::NoProxy);
    mCurrentRouteDescription =
        QString("Direct -> SAM %1:%2").arg(mSamHost).arg(mSamPort);
    mConnectTimeoutMs = 5000;
    return;
  }

  QSettings settings(mConfigPath + "/application.ini", QSettings::IniFormat);
  settings.beginGroup("Network");
  NetworkSettingsCompat::migrateNetworkCompatibilityKeys(settings);

  bool useSocks5 =
      !mForceDirectProxy && settings.value("SOCKS5ProxyEnabled", false).toBool();
  mConnectTimeoutMs = NetworkSettingsCompat::readSamConnectTimeoutMs(settings);
  mCurrentRouteDescription =
      QString("Direct -> SAM %1:%2").arg(mSamHost).arg(mSamPort);

  if (useSocks5) {
    QString proxyHost = settings.value("SOCKS5ProxyHost", "").toString();
    int proxyPort = settings.value("SOCKS5ProxyPort", 1080).toInt();
    QString proxyUsername = settings.value("SOCKS5ProxyUsername", "").toString();
    QString proxyPassword = settings.value("SOCKS5ProxyPassword", "").toString();

    if (!proxyHost.isEmpty()) {
      QNetworkProxy proxy;
      proxy.setType(QNetworkProxy::Socks5Proxy);
      proxy.setHostName(proxyHost);
      proxy.setPort(proxyPort);

      if (!proxyUsername.isEmpty()) {
        proxy.setUser(proxyUsername);
        proxy.setPassword(proxyPassword);
      }

      mTcpSocket.setProxy(proxy);
      mCurrentRouteDescription =
          QString("SOCKS5 %1:%2 -> SAM %3:%4")
              .arg(proxyHost)
              .arg(proxyPort)
              .arg(mSamHost)
              .arg(mSamPort);
      emit signDebugMessages(
          QString("• [Stream ID: %1] Using SOCKS5 proxy: %2:%3")
              .arg(mID)
              .arg(proxyHost)
              .arg(proxyPort));
    } else {
      mTcpSocket.setProxy(QNetworkProxy::NoProxy);
      mCurrentRouteDescription =
          QString("Direct (SOCKS5 enabled without host) -> SAM %1:%2")
              .arg(mSamHost)
              .arg(mSamPort);
      emit signDebugMessages(
          QString("• [Stream ID: %1] SOCKS5ProxyEnabled=true but SOCKS5ProxyHost is empty, fallback to direct connection")
              .arg(mID));
    }
  } else {
    mTcpSocket.setProxy(QNetworkProxy::NoProxy);
  }

  emit signDebugMessages(
      QString("• [Stream ID: %1] Connect route: %2 (timeout=%3ms)")
          .arg(mID)
          .arg(mCurrentRouteDescription)
          .arg(mConnectTimeoutMs));

  settings.endGroup();
}

bool CI2PStream::doConnect(QString mDestination) {
  if (mMode != CONNECT) {
    return false;
  }

  this->mDestination = mDestination;
  this->mModeStreamConnect = true;
  this->mModeStreamAccept = false;

  if (mTcpSocket.state() != QAbstractSocket::UnconnectedState)
    return false;

  configureSocks5Proxy();

  mTcpSocket.connectToHost(mSamHost, mSamPort.toInt());
  if (!mTcpSocket.waitForConnected(mConnectTimeoutMs)) {
    emit signDebugMessages(
        QString("• [Stream ID: %1] Connect failed [%2] %3 | route=%4")
            .arg(mID)
            .arg(classifySocketErrorCode(mTcpSocket.error()))
            .arg(mTcpSocket.errorString())
            .arg(mCurrentRouteDescription));
    slotDisconnected();
  }

  return true;
}

bool CI2PStream::doAccept() {
  if (mMode != ACCEPT) {
    return false;
  }

  this->mDestination = "";
  this->mModeStreamConnect = false;
  this->mModeStreamAccept = true;

  if (mTcpSocket.state() != QAbstractSocket::UnconnectedState)
    return false;

  configureSocks5Proxy();

  mTcpSocket.connectToHost(mSamHost, mSamPort.toInt());
  if (!mTcpSocket.waitForConnected(mConnectTimeoutMs)) {
    emit signDebugMessages(
        QString("• [Stream ID: %1] Accept failed [%2] %3 | route=%4")
            .arg(mID)
            .arg(classifySocketErrorCode(mTcpSocket.error()))
            .arg(mTcpSocket.errorString())
            .arg(mCurrentRouteDescription));
    slotDisconnected();
  }

  return true;
}

void CI2PStream::slotConnected() {
  QString smID = QString::number(mID, 10);
  const QByteArray handshakeMessage = buildHelloMessage();

  emit signDebugMessages("• [Stream ID: " + smID +
                         "] Controller ‣ Connected to SAM v3");
  mDoneDisconnect = false;
  emit signDebugMessages("• [Stream ID: " + smID + "] Outgoing ‣ " +
                         QString::fromUtf8(handshakeMessage));
  try {
    if (mTcpSocket.isWritable()) {
      mTcpSocket.write(handshakeMessage);
      // mTcpSocket.flush();
    }
  } catch (...) {
  }
}

void CI2PStream::slotDisconnected() {
  QString smID = QString::number(mID, 10);

  mStatusReceived = false;
  mDestinationReceived = false;
  mDoneDisconnect = false;
  mHandshakeSuccessful = false;
  mFIRSTPACKETCHAT_alreadySent = false;

  mUnKnownConnectionTimeout.stop();

  emit signDebugMessages("• [Stream ID: " + smID +
                         "] Controller ‣ Disconnected from SAM");
  emit signStreamStatusReceived(SAM_Message_Types::CLOSED, mID, "");
}

void CI2PStream::slotReadFromSocket() {
  using namespace SAM_Message_Types;
  QString smID = QString::number(mID, 10);
  QByteArray newData;
  if (mTcpSocket.state() == QTcpSocket::ConnectedState) {
    newData = mTcpSocket.readAll();
  } else {
    return;
  }

  /*
    QString debugData = newData.replace("STREAM STATUS RESULT=", "STATUS: ")
                            .replace("CANT_REACH_PEER MESSAGE=", "")
                            .replace("\"Connection timed out\"\n", "No
    Response");

    emit signDebugMessages("• [Stream ID: " + smID + "] Incoming ‣ " +
    debugData);
  */
  emit signDebugMessages("• [Stream ID: " + smID + "] Incoming ‣ " + newData);

  if (mHandshakeSuccessful == false) {

    mIncomingPackets->append(newData);
    if (mIncomingPackets->indexOf("\n", 0) == -1) {
      // Incomplete packet received???
      return;
    }

    QByteArray CurrentPacket;
    CurrentPacket =
        mIncomingPackets->left(mIncomingPackets->indexOf("\n", 0) + 1);

    mAnalyser = new CI2PSamMessageAnalyser("CI2PStream");

    QString t(CurrentPacket.data());
    SAM_MESSAGE sam = mAnalyser->Analyse(t);
    if (sam.type == HELLO_REPLAY && sam.result == OK) {
      mHandshakeSuccessful = true;
    }

    delete mAnalyser;
    mAnalyser = NULL;
    const QByteArray Data =
        buildStreamCommand(mStreamBridgeName, mDestination, mMode, mSilence);
    mIncomingPackets->remove(0, mIncomingPackets->indexOf("\n", 0) + 1);
    *(this) << Data;
  } else if (mStatusReceived == false) {
    mIncomingPackets->append(newData);
    if (mIncomingPackets->indexOf("\n", 0) == -1) {
      // Incomplete packet received???
      return;
    }

    QByteArray CurrentPacket;
    CurrentPacket =
        mIncomingPackets->left(mIncomingPackets->indexOf("\n", 0) + 1);

    // Get Stream Status
    mAnalyser = new CI2PSamMessageAnalyser("CI2PStream");

    QString t(CurrentPacket.data());

    SAM_MESSAGE sam = mAnalyser->Analyse(t);
    emit signStreamStatusReceived(sam.result, mID, sam.Message);

    delete mAnalyser;
    mAnalyser = NULL;
    mStatusReceived = true;

    mIncomingPackets->remove(0, mIncomingPackets->indexOf("\n", 0) + 1);
    if (mModeStreamConnect == true) {
      if (mIncomingPackets->length() != 0) {
        emit signDataReceived(mID, *(mIncomingPackets));
      }
    }

  } else if (mStatusReceived == true && mModeStreamAccept == true &&
             mDestinationReceived == false) {
    // get Destination
    mIncomingPackets->append(newData);
    if (mIncomingPackets->indexOf("\n", 0) == -1) {
      // Incomplete packet received???
      return;
    }

    QByteArray CurrentPacket;
    CurrentPacket =
        mIncomingPackets->left(mIncomingPackets->indexOf("\n", 0) + 1);

    mDestination = QString(CurrentPacket.data());
    mDestination = mDestination.trimmed();
    mDestinationReceived = true;

    mIncomingPackets->remove(0, mIncomingPackets->indexOf("\n", 0) + 1);

    emit signModeAcceptIncomingStream(mID);

    if (mIncomingPackets->length() != 0) {
      emit signDataReceived(mID, *(mIncomingPackets));
    }

    // start mUnKnownConnectionTimeout
    mUnKnownConnectionTimeout.start();
    emit signDebugMessages("• [Stream ID: " + smID +
                           "] Controller ‣ [Unknown] Connection Timeout");
    //--------------------------------
  } else {
    emit signDataReceived(mID, newData);
  }
}

void CI2PStream::doDisconnect() {
  mDoneDisconnect = true;
  if (mTcpSocket.bytesToWrite() > 0) {
    mTcpSocket.flush();
  }
  mTcpSocket.disconnectFromHost();
}

void CI2PStream::operator<<(const QByteArray Data) {
  QString smID = QString::number(mID, 10);

  if (mTcpSocket.state() == QTcpSocket::ConnectedState &&
      mHandshakeSuccessful) {
    emit signDebugMessages("• [Stream ID: " + smID + "] Outgoing ‣ " + Data);

    try {
      if (mTcpSocket.isWritable()) {
        mTcpSocket.write(Data);
        // mTcpSocket.flush();
      }
    } catch (...) {
    }
  } else {
    QByteArray Message = "• [Stream ID: ";
    Message += smID.toUtf8();
    Message += "] Controller ‣ Not connected";
    emit signDebugMessages(Message);
  }
}
void CI2PStream::operator<<(const QString Data) {
  QByteArray t = "";
  t.insert(0, Data.toUtf8());

  *(this) << t;
}

void CI2PStream::setConnectionType(const Type newTyp) {
  QString smID = QString::number(mID, 10);

  mConnectionType = newTyp;
  if (newTyp == KNOWN) {
    mUnKnownConnectionTimeout.stop();
    emit signDebugMessages("• [Stream ID: " + smID +
                           "] Controller ‣ Connection Type changed to KNOWN");
  }
}

void CI2PStream::startUnlimintedReconnect(qint32 msec) {
  if (mTimer == NULL) {
    mTimer = new QTimer();
    connect(mTimer, SIGNAL(timeout()), this, SLOT(slotCheckForReconnect()));
  }
  mTimer->start(msec);
}

void CI2PStream::stopUnlimintedReconnect() {
  mTimer->stop();
  delete mTimer;
  mTimer = NULL;
}

void CI2PStream::slotCheckForReconnect() {
  mTimer->stop();
  if (mMode == CONNECT) {
    doConnect(mDestination);
  }

  mTimer->start();
}

void CI2PStream::slotInitConnectionTimeout() {
  QString smID = QString::number(mID, 10);
  emit signStreamStatusReceived(SAM_Message_Types::CLOSED, mID, QString(""));
  emit signDebugMessages(
      "• [Stream ID: " + smID +
      "] Controller ‣ Disconnected after Initialization Timeout");
  doDisconnect();
}

void CI2PStream::setFIRSTPACKETCHAT_alreadySent(bool theValue) {
  mFIRSTPACKETCHAT_alreadySent = theValue;
}

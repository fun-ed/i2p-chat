#include "ConnectionManager.h"
#include "Core.h"
#include "RouterConnection.h"
#include "form_settingsgui.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QStackedWidget>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <signal.h>

namespace {

QString environmentString(const char *name) {
  return QString::fromLocal8Bit(qgetenv(name));
}

QString argumentValue(const QStringList &arguments, const QString &option) {
  const int index = arguments.indexOf(option);
  return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1)
                                                     : QString();
}

bool writeFile(const QString &path, const QByteArray &content,
               QIODevice::OpenMode mode = QIODevice::WriteOnly) {
  QFile file(path);
  if (!file.open(mode))
    return false;
  return file.write(content) == content.size();
}

int readIntegerFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return 0;
  bool ok = false;
  const int value = QString::fromUtf8(file.readAll()).trimmed().toInt(&ok);
  return ok ? value : 0;
}

int fakeSshMain(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const QStringList allArguments = app.arguments();
  const QStringList arguments = allArguments.mid(2);
  const QString stateDirectory = environmentString("I2PCHAT_FAKE_SSH_STATE");
  const QString mode = environmentString("I2PCHAT_FAKE_SSH_MODE");
  const QString socketPath = argumentValue(arguments, QStringLiteral("-S"));
  const int controlIndex = arguments.indexOf(QStringLiteral("-O"));
  const QString controlOperation =
      controlIndex >= 0 && controlIndex + 1 < arguments.size()
          ? arguments.at(controlIndex + 1)
          : QString();

  if (arguments.contains(QStringLiteral("-M"))) {
    const QString countPath = stateDirectory + QStringLiteral("/master-count");
    const int attempt = readIntegerFile(countPath) + 1;
    writeFile(countPath, QByteArray::number(attempt));
    QFile invocation(stateDirectory + QStringLiteral("/master-argv"));
    if (invocation.open(QIODevice::WriteOnly | QIODevice::Append)) {
      for (const QString &argument : arguments) {
        invocation.write(argument.toUtf8());
        invocation.write("\n");
      }
      invocation.write("--end-master--\n");
    }

    if (mode == QStringLiteral("auth")) {
      fprintf(stderr, "Permission denied (publickey).\n");
      return 255;
    }
    if (mode == QStringLiteral("hostkey")) {
      fprintf(stderr, "Host key verification failed.\n");
      return 255;
    }
    if (mode == QStringLiteral("config")) {
      fprintf(stderr, "Bad configuration option: FixtureOnly\n");
      return 255;
    }
    if (mode == QStringLiteral("unsupported")) {
      fprintf(stderr, "unknown option -- M\n");
      return 255;
    }
    if (mode == QStringLiteral("transient")) {
      const int failCount = qEnvironmentVariableIntValue("I2PCHAT_FAKE_FAIL_COUNT");
      if (attempt <= failCount) {
        fprintf(stderr, "connect to host failed: Connection timed out\n");
        return 255;
      }
    }

    const QString masterMarker = socketPath + QStringLiteral(".master");
    const QString masterPid = stateDirectory + QStringLiteral("/master-pid");
    const int startDelay =
        qEnvironmentVariableIntValue("I2PCHAT_FAKE_START_DELAY_MS");
    QTimer::singleShot(qMax(0, startDelay), &app, [masterMarker, masterPid]() {
      writeFile(masterMarker, QByteArrayLiteral("owned"));
      writeFile(masterPid,
                QByteArray::number(QCoreApplication::applicationPid()));
    });

    QTcpServer forwarder;
    QString requestedTargetHost;
    quint16 requestedTargetPort = 0;
    QTimer requestPoller;
    requestPoller.setInterval(20);
    QObject::connect(&requestPoller, &QTimer::timeout, &app,
                     [&forwarder, &requestedTargetHost, &requestedTargetPort,
                      socketPath]() {
                       const QString requestPath =
                           socketPath + QStringLiteral(".request");
                       QFile request(requestPath);
                       if (!request.open(QIODevice::ReadOnly))
                         return;
                       const QStringList fields =
                           QString::fromUtf8(request.readAll()).trimmed().split('|');
                       request.close();
                       QFile::remove(requestPath);
                       bool localPortOk = false;
                       bool remotePortOk = false;
                       const quint16 localPort =
                           fields.value(0).toUShort(&localPortOk);
                       requestedTargetHost = fields.value(1);
                       requestedTargetPort =
                           fields.value(2).toUShort(&remotePortOk);
                       const QString ackPath = socketPath + QStringLiteral(".ack");
                       if (fields.size() != 3 || !localPortOk || !remotePortOk ||
                           !forwarder.listen(QHostAddress(QStringLiteral("127.0.0.1")),
                                             localPort)) {
                         writeFile(ackPath, QByteArrayLiteral("BIND"));
                         return;
                       }
                       writeFile(ackPath, QByteArrayLiteral("OK"));
                     });
    QObject::connect(&forwarder, &QTcpServer::newConnection, &app,
                     [&app, &forwarder, &requestedTargetHost,
                      &requestedTargetPort]() {
                       while (forwarder.hasPendingConnections()) {
                         QTcpSocket *local = forwarder.nextPendingConnection();
                         QTcpSocket *remote = new QTcpSocket(&app);
                         QObject::connect(local, &QTcpSocket::readyRead, remote,
                                          [local, remote]() {
                                            remote->write(local->readAll());
                                          });
                         QObject::connect(remote, &QTcpSocket::readyRead, local,
                                          [local, remote]() {
                                            local->write(remote->readAll());
                                          });
                         QObject::connect(local, &QTcpSocket::disconnected,
                                          remote, &QObject::deleteLater);
                         QObject::connect(remote, &QTcpSocket::disconnected,
                                          local, &QObject::deleteLater);
                         remote->connectToHost(requestedTargetHost,
                                               requestedTargetPort);
                       }
                     });
    requestPoller.start();

    const int flapMs = qEnvironmentVariableIntValue("I2PCHAT_FAKE_FLAP_MS");
    if (mode == QStringLiteral("flap") && flapMs > 0)
      QTimer::singleShot(flapMs, &app, []() { QCoreApplication::quit(); });
    return app.exec();
  }

  if (controlOperation == QStringLiteral("check")) {
    if (mode == QStringLiteral("checkunsupported")) {
      fprintf(stderr, "unknown option -- O\n");
      return 255;
    }
    return QFile::exists(socketPath + QStringLiteral(".master")) ? 0 : 1;
  }

  if (controlOperation == QStringLiteral("forward")) {
    if (mode == QStringLiteral("forwardfail")) {
      fprintf(stderr, "bind: Address already in use\n");
      return 255;
    }
    const QString forward = argumentValue(arguments, QStringLiteral("-L"));
    const QStringList fields = forward.split(':');
    if (fields.size() != 4) {
      fprintf(stderr, "invalid fixture forwarding argument\n");
      return 255;
    }
    const QByteArray request =
        (fields.at(1) + QLatin1Char('|') + fields.at(2) + QLatin1Char('|') +
         fields.at(3))
            .toUtf8();
    const QString requestPath = socketPath + QStringLiteral(".request");
    const QString ackPath = socketPath + QStringLiteral(".ack");
    QFile::remove(ackPath);
    if (!writeFile(requestPath, request))
      return 255;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
      QFile ack(ackPath);
      if (ack.open(QIODevice::ReadOnly)) {
        const QByteArray result = ack.readAll();
        if (result == QByteArrayLiteral("OK"))
          return 0;
        fprintf(stderr, "bind: Address already in use\n");
        return 255;
      }
      QThread::msleep(10);
    }
    fprintf(stderr, "forward acknowledgement timed out\n");
    return 255;
  }
  fprintf(stderr, "unsupported fixture SSH operation\n");
  return 255;
}

class SamFixture : public QTcpServer {
public:
  enum class HelloReply { Good, WrongService, WrongVersion, Oversized, Silent };

  explicit SamFixture(HelloReply reply = HelloReply::Good,
                      bool failSession = false, QObject *parent = nullptr)
      : QTcpServer(parent), mReply(reply), mFailSession(failSession) {
    QObject::connect(this, &QTcpServer::newConnection, this, [this]() {
      while (hasPendingConnections()) {
        QTcpSocket *socket = nextPendingConnection();
        socket->setProperty("fixture-buffer", QByteArray());
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
          QByteArray pending = socket->property("fixture-buffer").toByteArray();
          pending.append(socket->readAll());
          int newline = pending.indexOf('\n');
          while (newline >= 0) {
            const QByteArray line = pending.left(newline).trimmed();
            pending.remove(0, newline + 1);
            handleLine(socket, line);
            newline = pending.indexOf('\n');
          }
          socket->setProperty("fixture-buffer", pending);
        });
      }
    });
  }

  bool start() {
    return listen(QHostAddress(QStringLiteral("127.0.0.1")), 0);
  }

  quint16 port() const { return serverPort(); }

private:
  void handleLine(QTcpSocket *socket, const QByteArray &line) {
    if (line.startsWith("HELLO VERSION")) {
      switch (mReply) {
      case HelloReply::WrongService:
        socket->write("HTTP/1.0 200 OK\n");
        return;
      case HelloReply::WrongVersion:
        socket->write("HELLO REPLY RESULT=OK VERSION=3.0\n");
        return;
      case HelloReply::Oversized:
        socket->write(QByteArray(300, 'X') + '\n');
        return;
      case HelloReply::Silent:
        return;
      case HelloReply::Good:
        break;
      }
      QPointer<QTcpSocket> guardedSocket(socket);
      socket->write("HELLO REPLY RESULT=OK ");
      QTimer::singleShot(10, this, [guardedSocket]() {
        if (guardedSocket)
          guardedSocket->write("VERSION=3.1\n");
      });
      return;
    }
    if (line.startsWith("SESSION CREATE")) {
      socket->write(mFailSession ? "SESSION STATUS RESULT=I2P_ERROR\n"
                                 : "SESSION STATUS RESULT=OK\n");
      return;
    }
    if (line.startsWith("NAMING LOOKUP NAME=ME"))
      socket->write("NAMING REPLY RESULT=OK NAME=ME VALUE=fixture\n");
  }

  HelloReply mReply;
  bool mFailSession;
};

class Socks5Fixture : public QTcpServer {
public:
  explicit Socks5Fixture(QObject *parent = nullptr) : QTcpServer(parent) {
    QObject::connect(this, &QTcpServer::newConnection, this, [this]() {
      while (hasPendingConnections()) {
        QTcpSocket *client = nextPendingConnection();
        client->setProperty("socks-stage", 0);
        client->setProperty("socks-buffer", QByteArray());
        QObject::connect(client, &QTcpSocket::readyRead, this,
                         [this, client]() { handleClient(client); });
      }
    });
  }

  bool start() {
    return listen(QHostAddress(QStringLiteral("127.0.0.1")), 0);
  }

  int connectRequestCount() const { return mConnectRequestCount; }

private:
  void handleClient(QTcpSocket *client) {
    QByteArray pending = client->property("socks-buffer").toByteArray();
    pending.append(client->readAll());
    int stage = client->property("socks-stage").toInt();
    while (true) {
      if (stage == 0) {
        if (pending.size() < 2)
          break;
        const int methodCount = static_cast<unsigned char>(pending.at(1));
        if (pending.size() < methodCount + 2)
          break;
        if (pending.at(0) != 5) {
          client->disconnectFromHost();
          return;
        }
        pending.remove(0, methodCount + 2);
        client->write("\x05\x00", 2);
        stage = 1;
        continue;
      }

      if (stage != 1 || pending.size() < 4)
        break;
      const int addressType = static_cast<unsigned char>(pending.at(3));
      int addressLength = 0;
      if (addressType == 1) {
        addressLength = 4;
      } else if (addressType == 4) {
        addressLength = 16;
      } else if (addressType == 3) {
        if (pending.size() < 5)
          break;
        addressLength = 1 + static_cast<unsigned char>(pending.at(4));
      } else {
        client->disconnectFromHost();
        return;
      }
      const int requestLength = 4 + addressLength + 2;
      if (pending.size() < requestLength)
        break;

      QString host;
      if (addressType == 1) {
        host = QStringLiteral("%1.%2.%3.%4")
                   .arg(static_cast<unsigned char>(pending.at(4)))
                   .arg(static_cast<unsigned char>(pending.at(5)))
                   .arg(static_cast<unsigned char>(pending.at(6)))
                   .arg(static_cast<unsigned char>(pending.at(7)));
      } else if (addressType == 3) {
        const int domainLength = static_cast<unsigned char>(pending.at(4));
        host = QString::fromUtf8(pending.mid(5, domainLength));
      } else {
        Q_IPV6ADDR ipv6;
        for (int index = 0; index < 16; ++index)
          ipv6.c[index] = static_cast<unsigned char>(pending.at(4 + index));
        QHostAddress address(ipv6);
        host = address.toString();
      }
      const int portOffset = 4 + addressLength;
      const quint16 port =
          static_cast<quint16>((static_cast<unsigned char>(pending.at(portOffset)) << 8) |
                               static_cast<unsigned char>(pending.at(portOffset + 1)));
      pending.remove(0, requestLength);
      client->setProperty("socks-stage", 2);
      ++mConnectRequestCount;
      client->write(QByteArray::fromHex("050000017f0000010000"));

      client->disconnect(this);
      QTcpSocket *upstream = new QTcpSocket(this);
      QObject::connect(client, &QTcpSocket::readyRead, upstream,
                       [client, upstream]() { upstream->write(client->readAll()); });
      QObject::connect(upstream, &QTcpSocket::readyRead, client,
                       [client, upstream]() { client->write(upstream->readAll()); });
      QObject::connect(client, &QTcpSocket::disconnected, upstream,
                       &QObject::deleteLater);
      upstream->connectToHost(host, port);
      return;
    }
    client->setProperty("socks-stage", stage);
    client->setProperty("socks-buffer", pending);
  }

  int mConnectRequestCount = 0;
};

class FakePathScope {
public:
  explicit FakePathScope(const QString &binDirectory) : mOldPath(qgetenv("PATH")) {
    const QByteArray prefix = QFile::encodeName(binDirectory) + ":";
    qputenv("PATH", prefix + mOldPath);
  }
  ~FakePathScope() { qputenv("PATH", mOldPath); }

private:
  QByteArray mOldPath;
};

} // namespace

class RemoteConnectionTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
#ifdef Q_OS_WIN
    QSKIP("The fake OpenSSH process fixture requires a POSIX shell.");
#endif
    QVERIFY(mTestRoot.isValid());
    mFakeBin = mTestRoot.path() + QStringLiteral("/fake-bin");
    QVERIFY(QDir().mkpath(mFakeBin));
    mFakeSsh = mFakeBin + QStringLiteral("/ssh");
    const QByteArray script =
        "#!/bin/sh\nexec \"$I2PCHAT_FAKE_SSH_EXE\" --fake-ssh \"$@\"\n";
    QVERIFY(writeFile(mFakeSsh, script));
    QVERIFY(QFile::setPermissions(
        mFakeSsh, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                      QFileDevice::ExeOwner));
    qputenv("I2PCHAT_FAKE_SSH_EXE",
            QFile::encodeName(QCoreApplication::applicationFilePath()));
  }

  void init() {
    mStateDirectory = mTestRoot.path() + QStringLiteral("/state-") +
                      QUuid::createUuid().toString(QUuid::Id128);
    QVERIFY(QDir().mkpath(mStateDirectory));
    qputenv("I2PCHAT_FAKE_SSH_STATE", QFile::encodeName(mStateDirectory));
    qputenv("I2PCHAT_FAKE_SSH_MODE", "success");
    qputenv("I2PCHAT_FAKE_FAIL_COUNT", "0");
    qputenv("I2PCHAT_FAKE_FLAP_MS", "0");
    qputenv("I2PCHAT_FAKE_START_DELAY_MS", "0");
  }

  void optionsRoundTripKeepsLegacyNetworkSettings() {
    const QString iniPath = mTestRoot.path() + QStringLiteral("/round-trip.ini");
    QSettings settings(iniPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Network"));
    settings.setValue(QStringLiteral("ConnectionMode"), QStringLiteral("socks5"));
    settings.setValue(QStringLiteral("SAMHost"), QStringLiteral("127.0.0.1"));
    settings.setValue(QStringLiteral("SAMPort"), 7777);
    settings.setValue(QStringLiteral("SOCKS5ProxyEnabled"), true);
    settings.setValue(QStringLiteral("SOCKS5ProxyPassword"), QStringLiteral("fixture-only"));

    RouterConnectionOptions saved;
    saved.remote = true;
    saved.sshConfigFile = QStringLiteral("/tmp/fixture-ssh-config");
    saved.sshProfile = QStringLiteral("fixture");
    saved.remoteSamHost = QStringLiteral("sam.example.i2p");
    saved.remoteSamPort = 9876;
    saved.localForwardPort = 43210;
    saved.sshExecutable = QStringLiteral("must-not-persist");
    saved.saveNetworkSettings(settings);
    settings.endGroup();
    settings.sync();

    const RouterConnectionOptions loaded = RouterConnectionOptions::fromConfig(iniPath);
    QVERIFY(loaded.remote);
    QCOMPARE(loaded.sshConfigFile, saved.sshConfigFile);
    QCOMPARE(loaded.sshProfile, saved.sshProfile);
    QCOMPARE(loaded.remoteSamHost, saved.remoteSamHost);
    QCOMPARE(loaded.remoteSamPort, saved.remoteSamPort);
    QCOMPARE(loaded.localForwardPort, saved.localForwardPort);
    QCOMPARE(loaded.localSamHost, QStringLiteral("127.0.0.1"));
    QCOMPARE(loaded.localSamPort, quint16(7777));
    QVERIFY(loaded.sshExecutable.isEmpty());

    settings.beginGroup(QStringLiteral("Network"));
    QCOMPARE(settings.value(QStringLiteral("ConnectionMode")).toString(),
             QStringLiteral("socks5"));
    QCOMPARE(settings.value(QStringLiteral("SAMPort")).toInt(), 7777);
    QCOMPARE(settings.value(QStringLiteral("SOCKS5ProxyPassword")).toString(),
             QStringLiteral("fixture-only"));
    settings.endGroup();
  }

  void sshProfilesListOnlyLiteralRootHostEntries() {
    const QString configPath = mTestRoot.path() + QStringLiteral("/profiles.conf");
    const QByteArray config =
        "Host first second *.wild ?pattern !negated\n"
        "  HostName example.invalid\n"
        "Include /path/that/must/not-be-read\n"
        "Match exec \"touch /tmp/never-run\"\n"
        "  User fixture\n"
        "Host after-match\n"
        "Host first\n";
    QVERIFY(writeFile(configPath, config));
    QString error;
    const QStringList profiles = RouterConnectionOptions::sshProfiles(configPath, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(profiles, QStringList({QStringLiteral("first"),
                                    QStringLiteral("second"),
                                    QStringLiteral("after-match")}));
  }

  void validationAndCommandsKeepArgumentsSeparateAndStrict() {
    const QString configPath = writeSshConfig();
    RouterConnectionOptions options = remoteOptions(configPath, 12345);
    QString error;
    const QStringList master = options.masterArguments(
        QStringLiteral("/tmp/i2pc-test/s"), &error);
    QVERIFY(error.isEmpty());
    QVERIFY(master.contains(QStringLiteral("-M")));
    QVERIFY(master.contains(QStringLiteral("-N")));
    QVERIFY(master.contains(QStringLiteral("-T")));
    QVERIFY(master.contains(QStringLiteral("-F")));
    QVERIFY(master.contains(QStringLiteral("BatchMode=yes")));
    QVERIFY(master.contains(QStringLiteral("StrictHostKeyChecking=yes")));
    QVERIFY(master.contains(QStringLiteral("UpdateHostKeys=no")));
    QVERIFY(master.contains(QStringLiteral("PasswordAuthentication=no")));
    QVERIFY(master.contains(QStringLiteral("KbdInteractiveAuthentication=no")));
    QVERIFY(master.contains(QStringLiteral("PreferredAuthentications=publickey")));
    QVERIFY(master.contains(QStringLiteral("ForwardAgent=no")));
    QVERIFY(master.contains(QStringLiteral("ForwardX11=no")));
    QVERIFY(master.contains(QStringLiteral("RemoteCommand=none")));
    QVERIFY(master.contains(QStringLiteral("LocalCommand=none")));
    QVERIFY(master.contains(QStringLiteral("ForkAfterAuthentication=no")));
    QVERIFY(master.contains(QStringLiteral("ControlPersist=no")));
    QVERIFY(master.contains(QStringLiteral("ClearAllForwardings=yes")));
    QVERIFY(master.contains(QStringLiteral("ExitOnForwardFailure=yes")));
    QVERIFY(!master.contains(QStringLiteral("-L")));
    QVERIFY(!master.join(QLatin1Char(' ')).contains(QStringLiteral(";")));

    const QStringList forward = options.forwardArguments(
        QStringLiteral("/tmp/i2pc-test/s"), 7654, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(forward.contains(QStringLiteral("-O")));
    QVERIFY(forward.contains(QStringLiteral("forward")));
    QVERIFY(forward.contains(QStringLiteral("ExitOnForwardFailure=yes")));
    QVERIFY(!forward.contains(QStringLiteral("ClearAllForwardings=yes")));
    QVERIFY(forward.contains(QStringLiteral("127.0.0.1:7654:127.0.0.1:12345")));

    options.sshProfile = QStringLiteral("alias -oProxyCommand=touch_bad");
    QVERIFY(!options.validationError().isEmpty());
    options.sshProfile = QStringLiteral("fixture");
    options.remoteSamHost = QStringLiteral("127.0.0.1:22 -L");
    QVERIFY(!options.validationError().isEmpty());
    options.remote = false;
    options.configurationError = QStringLiteral("invalid remote-only setting");
    QVERIFY(options.validationError().isEmpty());
    options.remote = true;
    QVERIFY(!options.validationError().isEmpty());
    options.configurationError.clear();
  }

  void localSamProbe_data() {
    QTest::addColumn<int>("replyValue");
    QTest::addColumn<bool>("expectedSuccess");
    QTest::newRow("partial-valid")
        << static_cast<int>(SamFixture::HelloReply::Good) << true;
    QTest::newRow("wrong-service")
        << static_cast<int>(SamFixture::HelloReply::WrongService) << false;
    QTest::newRow("wrong-version")
        << static_cast<int>(SamFixture::HelloReply::WrongVersion) << false;
    QTest::newRow("oversized")
        << static_cast<int>(SamFixture::HelloReply::Oversized) << false;
    QTest::newRow("timeout")
        << static_cast<int>(SamFixture::HelloReply::Silent) << false;
  }

  void localSamProbe() {
    QFETCH(int, replyValue);
    QFETCH(bool, expectedSuccess);
    const auto reply = static_cast<SamFixture::HelloReply>(replyValue);
    SamFixture fixture(reply);
    QVERIFY(fixture.start());

    RouterConnectionController controller;
    QSignalSpy healthSpy(&controller,
                         &RouterConnectionController::signHealthChecked);
    controller.checkLocal(QStringLiteral("127.0.0.1"), fixture.port(), QString());
    QTRY_COMPARE_WITH_TIMEOUT(healthSpy.count(), 1, 7000);
    QCOMPARE(healthSpy.constFirst().at(0).toBool(), expectedSuccess);
    if (expectedSuccess)
      QVERIFY(!healthSpy.constFirst().at(1).toString().contains(
          QStringLiteral("tunnel"), Qt::CaseInsensitive));
    QVERIFY(!controller.isBusy());
  }

  void localHealthUsesLegacySocksProxy() {
    SamFixture sam;
    Socks5Fixture proxy;
    QVERIFY(sam.start());
    QVERIFY(proxy.start());
    const QString iniPath =
        mTestRoot.path() + QStringLiteral("/proxy-health.ini");
    QSettings settings(iniPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Network"));
    settings.setValue(QStringLiteral("SOCKS5ProxyEnabled"), true);
    settings.setValue(QStringLiteral("SOCKS5ProxyHost"),
                      QStringLiteral("127.0.0.1"));
    settings.setValue(QStringLiteral("SOCKS5ProxyPort"), proxy.serverPort());
    settings.endGroup();
    settings.sync();

    RouterConnectionController controller;
    QSignalSpy healthSpy(&controller,
                         &RouterConnectionController::signHealthChecked);
    controller.checkLocal(QStringLiteral("127.0.0.1"), sam.port(), iniPath);
    QTRY_COMPARE_WITH_TIMEOUT(healthSpy.count(), 1, 5000);
    QVERIFY(healthSpy.constFirst().at(0).toBool());
    QCOMPARE(proxy.connectRequestCount(), 1);
  }

  void remoteForwardAcknowledgementHealthAndNoSecondMaster() {
    SamFixture fixture;
    QVERIFY(fixture.start());
    RouterConnectionController controller;
    RouterConnectionOptions options = remoteOptions(writeSshConfig(), fixture.port());
    options.sshExecutable = mFakeSsh;
    QSignalSpy readySpy(&controller, &RouterConnectionController::signReady);
    QSignalSpy failureSpy(&controller, &RouterConnectionController::signFailed);
    QSignalSpy healthSpy(&controller,
                         &RouterConnectionController::signHealthChecked);

    controller.startRemote(options, true);
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 10000);
    QVERIFY(controller.hasRemoteTunnel());
    QCOMPARE(failureSpy.count(), 0);
    controller.checkActiveRemote();
    QTRY_COMPARE_WITH_TIMEOUT(healthSpy.count(), 1, 5000);
    QVERIFY(healthSpy.constFirst().at(0).toBool());
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);
    controller.stop();
    QVERIFY(!controller.hasRemoteTunnel());
    verifyFakeMasterExited();
  }

  void remoteOneShotHealthClosesItsOwnedTunnel() {
    SamFixture fixture;
    QVERIFY(fixture.start());
    RouterConnectionController controller;
    RouterConnectionOptions options = remoteOptions(writeSshConfig(), fixture.port());
    options.sshExecutable = mFakeSsh;
    QSignalSpy healthSpy(&controller,
                         &RouterConnectionController::signHealthChecked);

    controller.startRemote(options, false);
    QTRY_COMPARE_WITH_TIMEOUT(healthSpy.count(), 1, 10000);
    QVERIFY(healthSpy.constFirst().at(0).toBool());
    QVERIFY(!controller.hasRemoteTunnel());
    verifyFakeMasterExited();
  }

  void remoteSecurityAndProcessFailuresAreTerminal_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("authentication") << QStringLiteral("auth");
    QTest::newRow("host-key") << QStringLiteral("hostkey");
    QTest::newRow("config") << QStringLiteral("config");
    QTest::newRow("unsupported-multiplexing")
        << QStringLiteral("unsupported");
    QTest::newRow("unsupported-control-check")
        << QStringLiteral("checkunsupported");
    QTest::newRow("forward-bind") << QStringLiteral("forwardfail");
  }

  void remoteSecurityAndProcessFailuresAreTerminal() {
    QFETCH(QString, mode);
    qputenv("I2PCHAT_FAKE_SSH_MODE", mode.toLocal8Bit());
    RouterConnectionController controller;
    RouterConnectionOptions options = remoteOptions(writeSshConfig(), 7656);
    options.sshExecutable = mFakeSsh;
    QSignalSpy failureSpy(&controller, &RouterConnectionController::signFailed);
    controller.startRemote(options, false);
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 5000);
    QVERIFY(failureSpy.constFirst().at(1).toBool());
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);
  }

  void settingsRouteRoundTripAndConnectionPageFits() {
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    CCore core(configDirectory);
    {
      form_settingsgui dialog(core);
      dialog.setAttribute(Qt::WA_DeleteOnClose, false);
      auto *mode = dialog.findChild<QComboBox *>("routerModeCombo");
      auto *profile = dialog.findChild<QComboBox *>("sshProfileCombo");
      auto *host = dialog.findChild<QLineEdit *>("remoteSamHostEdit");
      auto *port = dialog.findChild<QSpinBox *>("remoteSamPortSpinBox");
      auto *forward = dialog.findChild<QSpinBox *>("localForwardPortSpinBox");
      auto *stack = dialog.findChild<QStackedWidget *>("Settings");
      auto *page = dialog.findChild<QWidget *>("connection_Page");
      QVERIFY(mode && profile && host && port && forward && stack && page);
      mode->setCurrentIndex(mode->findData(QStringLiteral("remote")));
      profile->setEditText(QStringLiteral("saved-profile"));
      host->setText(QStringLiteral("::1"));
      port->setValue(17656);
      forward->setValue(27656);
      stack->setCurrentWidget(page);
      dialog.show();
      QApplication::processEvents();
      for (const QString &name : {QStringLiteral("routerOptionsGroup"),
                                  QStringLiteral("routerHealthCheckButton"),
                                  QStringLiteral("healthScopeLabel")}) {
        QWidget *control = dialog.findChild<QWidget *>(name);
        QVERIFY(control && control->isVisible());
        const QRect bounds(control->mapTo(page, QPoint()), control->size());
        QVERIFY2(page->rect().contains(bounds), qPrintable(name));
      }
      QVERIFY(QMetaObject::invokeMethod(&dialog, "saveSettings",
                                       Qt::DirectConnection));
    }
    const RouterConnectionOptions saved = RouterConnectionOptions::fromConfig(
        configDirectory + QStringLiteral("/application.ini"));
    QVERIFY(saved.remote);
    QCOMPARE(saved.sshProfile, QStringLiteral("saved-profile"));
    QCOMPARE(saved.remoteSamHost, QStringLiteral("::1"));
    QCOMPARE(saved.remoteSamPort, quint16(17656));
    QCOMPARE(saved.localForwardPort, quint16(27656));
    {
      form_settingsgui dialog(core);
      dialog.setAttribute(Qt::WA_DeleteOnClose, false);
      QCOMPARE(dialog.findChild<QComboBox *>("sshProfileCombo")->currentText(),
               saved.sshProfile);
      QCOMPARE(dialog.findChild<QLineEdit *>("remoteSamHostEdit")->text(),
               saved.remoteSamHost);
      auto *mode = dialog.findChild<QComboBox *>("routerModeCombo");
      mode->setCurrentIndex(mode->findData(QStringLiteral("local")));
      QVERIFY(QMetaObject::invokeMethod(&dialog, "saveSettings",
                                       Qt::DirectConnection));
    }
    QVERIFY(!RouterConnectionOptions::fromConfig(
                 configDirectory + QStringLiteral("/application.ini")).remote);
    QCOMPARE(core.getOnlineStatus(), User::USEROFFLINE);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 0);
  }

  void settingsSaveDoesNotPersistRouteDraftDuringConnectOrHealthCheck() {
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);

    {
      qputenv("I2PCHAT_FAKE_START_DELAY_MS", "2500");
      CCore core(configDirectory);
      form_settingsgui dialog(core);
      dialog.setAttribute(Qt::WA_DeleteOnClose, false);
      auto *profile =
          dialog.findChild<QComboBox *>(QStringLiteral("sshProfileCombo"));
      auto *statusLabel =
          dialog.findChild<QLabel *>(QStringLiteral("routerConnectionStatusLabel"));
      QVERIFY(profile);
      QVERIFY(statusLabel);
      profile->setEditText(QStringLiteral("draft-before-connect"));

      core.setOnlineStatus(User::USERONLINE);
      QCOMPARE(core.getOnlineStatus(), User::USERTRYTOCONNECT);
      QCOMPARE(statusLabel->text(), core.routerConnectionStatusText());
      profile->setEditText(QStringLiteral("stale-draft-while-online"));
      QVERIFY(!profile->isEnabled());
      QVERIFY(QMetaObject::invokeMethod(&dialog, "saveSettings",
                                        Qt::DirectConnection));
      core.setOnlineStatus(User::USEROFFLINE);
    }
    {
      QSettings settings(configDirectory + QStringLiteral("/application.ini"),
                         QSettings::IniFormat);
      settings.beginGroup(QStringLiteral("Network"));
      QCOMPARE(settings.value(QStringLiteral("SSHProfile")).toString(),
               QStringLiteral("fixture"));
      settings.endGroup();
    }

    {
      qputenv("I2PCHAT_FAKE_START_DELAY_MS", "2500");
      CCore core(configDirectory);
      form_settingsgui dialog(core);
      dialog.setAttribute(Qt::WA_DeleteOnClose, false);
      auto *profile =
          dialog.findChild<QComboBox *>(QStringLiteral("sshProfileCombo"));
      auto *statusLabel =
          dialog.findChild<QLabel *>(QStringLiteral("routerConnectionStatusLabel"));
      auto *healthButton =
          dialog.findChild<QPushButton *>(QStringLiteral("routerHealthCheckButton"));
      QVERIFY(profile);
      QVERIFY(statusLabel);
      QVERIFY(healthButton);
      profile->setEditText(QStringLiteral("draft-during-health"));
      QVERIFY(QMetaObject::invokeMethod(&dialog, "checkRouterHealth",
                                        Qt::DirectConnection));
      QVERIFY(core.routerConnectionHealthBusy());
      QVERIFY(!healthButton->isEnabled());
      QCOMPARE(statusLabel->text(), core.routerConnectionStatusText());
      QVERIFY(QMetaObject::invokeMethod(&dialog, "saveSettings",
                                        Qt::DirectConnection));
      core.setOnlineStatus(User::USEROFFLINE);
    }
    {
      QSettings settings(configDirectory + QStringLiteral("/application.ini"),
                         QSettings::IniFormat);
      settings.beginGroup(QStringLiteral("Network"));
      QCOMPARE(settings.value(QStringLiteral("SSHProfile")).toString(),
               QStringLiteral("fixture"));
      settings.endGroup();
    }
  }

  void missingSshAndForwardPortConflictFailClearly() {
    RouterConnectionOptions missing = remoteOptions(writeSshConfig(), 7656);
    missing.sshExecutable = mTestRoot.path() + QStringLiteral("/missing-ssh");
    RouterConnectionController missingController;
    QSignalSpy missingSpy(&missingController,
                          &RouterConnectionController::signFailed);
    missingController.startRemote(missing, false);
    QTRY_COMPARE_WITH_TIMEOUT(missingSpy.count(), 1, 3000);
    QVERIFY(missingSpy.constFirst().at(1).toBool());

    QTcpServer occupied;
    QVERIFY(occupied.listen(QHostAddress(QStringLiteral("127.0.0.1")), 0));
    RouterConnectionOptions conflict = remoteOptions(writeSshConfig(), 7656);
    conflict.localForwardPort = occupied.serverPort();
    conflict.sshExecutable = mFakeSsh;
    RouterConnectionController conflictController;
    QSignalSpy conflictSpy(&conflictController,
                           &RouterConnectionController::signFailed);
    conflictController.startRemote(conflict, false);
    QTRY_COMPARE_WITH_TIMEOUT(conflictSpy.count(), 1, 1000);
    QVERIFY(conflictSpy.constFirst().at(1).toBool());
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 0);
  }

  void coreWaitsForForwardAndSamThenUsesOneMaster() {
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);
    QSignalSpy healthSpy(&core, &CCore::signRouterHealthChecked);

    core.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(core.getOnlineStatus(), User::USERONLINE, 12000);
    QVERIFY(!core.getConnectionManager()->isComponentStopped());
    QVERIFY(core.getConnectionManager()->getAllStreamIncomingListenerObjects()->size() > 0);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);

    const RouterConnectionOptions options = RouterConnectionOptions::fromConfig(
        configDirectory + QStringLiteral("/application.ini"));
    core.checkRouterConnection(options);
    QTRY_COMPARE_WITH_TIMEOUT(healthSpy.count(), 1, 5000);
    QVERIFY(healthSpy.constFirst().at(0).toBool());
    QVERIFY(core.routerConnectionStatusText().contains(
        QStringLiteral("Remote SAM session ready")));
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);

    core.setOnlineStatus(User::USEROFFLINE);
    QCOMPARE(core.getOnlineStatus(), User::USEROFFLINE);
    verifyFakeMasterExited();
  }

  void duplicateQueuedSessionFailuresOnlyScheduleOneRetry() {
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);
    QSignalSpy statusSpy(&core, &CCore::signRouterConnectionStatusChanged);

    core.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(core.getOnlineStatus(), User::USERONLINE, 12000);
    core.getConnectionManager()->signStreamControllerStatusOK(false);
    core.getConnectionManager()->signStreamControllerStatusOK(false);
    QTRY_VERIFY_WITH_TIMEOUT(
        core.routerConnectionStatusText().contains(QStringLiteral("Retrying")),
        3000);
    QTRY_COMPARE_WITH_TIMEOUT(core.getOnlineStatus(), User::USERONLINE, 12000);

    int retryStatusCount = 0;
    for (int index = 0; index < statusSpy.count(); ++index) {
      if (statusSpy.at(index).constFirst().toString().contains(
              QStringLiteral("Retrying")))
        ++retryStatusCount;
    }
    QCOMPARE(retryStatusCount, 1);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 2);
    core.setOnlineStatus(User::USEROFFLINE);
    verifyFakeMasterExited();
  }

  void remoteRetryKeepsItsRouteSnapshotAcrossConfigChanges() {
    qputenv("I2PCHAT_FAKE_SSH_MODE", "transient");
    qputenv("I2PCHAT_FAKE_FAIL_COUNT", "1");
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    {
      QSettings settings(configDirectory + QStringLiteral("/application.ini"),
                         QSettings::IniFormat);
      settings.beginGroup(QStringLiteral("Network"));
      settings.setValue(QStringLiteral("SAMPort"), fixture.port());
      settings.endGroup();
      settings.sync();
    }

    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);
    core.setOnlineStatus(User::USERONLINE);
    QTRY_VERIFY_WITH_TIMEOUT(
        core.routerConnectionStatusText().contains(QStringLiteral("Retrying")),
        5000);
    {
      QSettings settings(configDirectory + QStringLiteral("/application.ini"),
                         QSettings::IniFormat);
      settings.beginGroup(QStringLiteral("Network"));
      settings.setValue(QStringLiteral("RouterMode"), QStringLiteral("local"));
      settings.setValue(QStringLiteral("SSHProfile"),
                        QStringLiteral("changed-after-start"));
      settings.endGroup();
      settings.sync();
    }

    QTRY_COMPARE_WITH_TIMEOUT(core.getOnlineStatus(), User::USERONLINE, 12000);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 2);
    QVERIFY(!core.getConnectionManager()->isComponentStopped());
    core.setOnlineStatus(User::USEROFFLINE);
    verifyFakeMasterExited();
  }

  void coreSamSessionFailureDoesNotCreateListenerAndRetriesAreFinite() {
    SamFixture fixture(SamFixture::HelloReply::Good, true);
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);

    core.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(core.getOnlineStatus(), User::USEROFFLINE, 15000);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 4);
    QVERIFY(core.getConnectionManager()->getAllStreamIncomingListenerObjects()->isEmpty());
  }

  void coreAuthenticationConfigAndHostKeyErrorsDoNotRetry_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("authentication") << QStringLiteral("auth");
    QTest::newRow("host-key") << QStringLiteral("hostkey");
    QTest::newRow("config") << QStringLiteral("config");
  }

  void coreAuthenticationConfigAndHostKeyErrorsDoNotRetry() {
    QFETCH(QString, mode);
    qputenv("I2PCHAT_FAKE_SSH_MODE", mode.toLocal8Bit());
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);

    core.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(core.getOnlineStatus(), User::USEROFFLINE, 5000);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);
  }

  void offlineCancelsStartupAndHealthProbe() {
    qputenv("I2PCHAT_FAKE_SSH_MODE", "success");
    qputenv("I2PCHAT_FAKE_START_DELAY_MS", "3000");
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);

    core.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")),
                              1, 4000);
    QVERIFY(core.getConnectionManager()->isComponentStopped());
    core.setOnlineStatus(User::USEROFFLINE);
    QTest::qWait(300);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);
    verifyFakeMasterExited();

    qputenv("I2PCHAT_FAKE_START_DELAY_MS", "0");
    SamFixture silent(SamFixture::HelloReply::Silent);
    QVERIFY(silent.start());
    const QString healthConfig = makeCoreConfig(silent.port());
    CCore healthCore(healthConfig);
    QSignalSpy healthSpy(&healthCore, &CCore::signRouterHealthChecked);
    const RouterConnectionOptions options = RouterConnectionOptions::fromConfig(
        healthConfig + QStringLiteral("/application.ini"));
    healthCore.checkRouterConnection(options);
    QTRY_VERIFY_WITH_TIMEOUT(
        healthCore.routerConnectionStatusText().contains(QStringLiteral("Checking SAM HELLO")),
        8000);
    QVERIFY(healthCore.routerConnectionHealthBusy());
    healthCore.setOnlineStatus(User::USEROFFLINE);
    QTest::qWait(200);
    QCOMPARE(healthSpy.count(), 0);
    verifyFakeMasterExited();
  }

  void offlineCancelsBackoffAndBriefReadinessDoesNotResetRetryBudget() {
    qputenv("I2PCHAT_FAKE_SSH_MODE", "transient");
    qputenv("I2PCHAT_FAKE_FAIL_COUNT", "4");
    SamFixture fixture;
    QVERIFY(fixture.start());
    const QString configDirectory = makeCoreConfig(fixture.port());
    FakePathScope pathScope(mFakeBin);
    CCore core(configDirectory);
    core.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")),
                              1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        core.routerConnectionStatusText().contains(QStringLiteral("Retrying")), 5000);
    core.setOnlineStatus(User::USEROFFLINE);
    QTest::qWait(1300);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 1);
    QCOMPARE(core.getOnlineStatus(), User::USEROFFLINE);
    writeFile(mStateDirectory + QStringLiteral("/master-count"), QByteArrayLiteral("0"));
    QFile::remove(mStateDirectory + QStringLiteral("/master-pid"));

    qputenv("I2PCHAT_FAKE_SSH_MODE", "flap");
    qputenv("I2PCHAT_FAKE_FLAP_MS", "1200");
    const QString flapConfig = makeCoreConfig(fixture.port());
    CCore flapCore(flapConfig);
    flapCore.setOnlineStatus(User::USERONLINE);
    QTRY_COMPARE_WITH_TIMEOUT(flapCore.getOnlineStatus(), User::USEROFFLINE, 18000);
    QCOMPARE(readIntegerFile(mStateDirectory + QStringLiteral("/master-count")), 4);
  }

private:
  QString writeSshConfig() const {
    const QString path = mTestRoot.path() + QStringLiteral("/ssh-") +
                         QUuid::createUuid().toString(QUuid::Id128) +
                         QStringLiteral(".conf");
    if (!writeFile(path, QByteArrayLiteral("Host fixture\n  HostName 127.0.0.1\n")))
      return QString();
    return path;
  }

  RouterConnectionOptions remoteOptions(const QString &configPath,
                                        quint16 remotePort) const {
    RouterConnectionOptions options;
    options.remote = true;
    options.sshConfigFile = configPath;
    options.sshProfile = QStringLiteral("fixture");
    options.remoteSamHost = QStringLiteral("127.0.0.1");
    options.remoteSamPort = remotePort;
    options.localSamHost = QStringLiteral("127.0.0.1");
    options.localSamPort = 7656;
    return options;
  }

  QString makeCoreConfig(quint16 remoteSamPort) const {
    const QString directory = mTestRoot.path() + QStringLiteral("/core-") +
                              QUuid::createUuid().toString(QUuid::Id128);
    if (!QDir().mkpath(directory))
      return QString();
    const QString sshConfig = directory + QStringLiteral("/ssh_config");
    if (!writeFile(sshConfig,
                   QByteArrayLiteral("Host fixture\n  HostName 127.0.0.1\n")))
      return QString();
    QSettings settings(directory + QStringLiteral("/application.ini"),
                       QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Network"));
    settings.setValue(QStringLiteral("RouterMode"), QStringLiteral("remote"));
    settings.setValue(QStringLiteral("SSHConfigFile"), sshConfig);
    settings.setValue(QStringLiteral("SSHProfile"), QStringLiteral("fixture"));
    settings.setValue(QStringLiteral("SSHRemoteSAMHost"), QStringLiteral("127.0.0.1"));
    settings.setValue(QStringLiteral("SSHRemoteSAMPort"), remoteSamPort);
    settings.setValue(QStringLiteral("SSHLocalForwardPort"), 0);
    settings.setValue(QStringLiteral("SAMHost"), QStringLiteral("127.0.0.1"));
    settings.setValue(QStringLiteral("SAMPort"), 7656);
    settings.setValue(QStringLiteral("SamPrivKey"), QStringLiteral("fixture-private-key"));
    settings.setValue(QStringLiteral("SOCKS5ProxyEnabled"), true);
    settings.setValue(QStringLiteral("SOCKS5ProxyHost"), QStringLiteral("127.0.0.1"));
    settings.setValue(QStringLiteral("SOCKS5ProxyPort"), 1);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("User-Infos"));
    settings.setValue(QStringLiteral("Nickname"), QStringLiteral("FixtureUser"));
    settings.endGroup();
    settings.sync();
    writeFile(directory + QStringLiteral("/users.config"), QByteArray());
    return directory;
  }

  void verifyFakeMasterExited() const {
#ifdef Q_OS_WIN
    QSKIP("The process cleanup assertion requires POSIX process signaling.");
#else
    const int pid = readIntegerFile(mStateDirectory + QStringLiteral("/master-pid"));
    if (pid <= 0)
      return;
    errno = 0;
    QVERIFY(::kill(pid, 0) == -1 && errno == ESRCH);
#endif
  }

  QTemporaryDir mTestRoot;
  QString mFakeBin;
  QString mFakeSsh;
  QString mStateDirectory;
};

int main(int argc, char **argv) {
  if (argc > 1 && QByteArray(argv[1]) == QByteArrayLiteral("--fake-ssh"))
    return fakeSshMain(argc, argv);
  QApplication application(argc, argv);
  RemoteConnectionTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "remote_connection_test.moc"

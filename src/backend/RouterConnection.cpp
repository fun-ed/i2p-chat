#include "RouterConnection.h"

#include "NetworkSettingsCompat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>

namespace {
constexpr int kSshStartupTimeoutMs = 15000;
constexpr int kSamProbeTimeoutMs = 5000;
constexpr int kSamReplyLimit = 256;
constexpr int kHealthIntervalMs = 30000;
constexpr int kControlCheckIntervalMs = 100;
constexpr int kStderrLimit = 4096;

QString optionHost(const QString &host) {
  QHostAddress address;
  if (address.setAddress(host) && address.protocol() == QAbstractSocket::IPv6Protocol)
    return QStringLiteral("[%1]").arg(host);
  return host;
}

bool validSamHost(const QString &host) {
  const QString value = host.trimmed();
  if (value.isEmpty() || value != host)
    return false;
  QHostAddress address;
  if (address.setAddress(value))
    return true;
  static const QRegularExpression hostname(
      QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$"));
  return hostname.match(value).hasMatch() && !value.contains(QStringLiteral(".."));
}

bool validProfile(const QString &profile) {
  static const QRegularExpression alias(
      QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$"));
  return alias.match(profile).hasMatch();
}

QString processProgram(const RouterConnectionOptions &options) {
  return options.sshExecutable.isEmpty() ? QStringLiteral("ssh")
                                         : options.sshExecutable;
}

void appendBounded(QByteArray &target, const QByteArray &bytes) {
  if (target.size() >= kStderrLimit || bytes.isEmpty())
    return;
  target.append(bytes.constData(), qMin(bytes.size(), kStderrLimit - target.size()));
}

void stopOwnedProcess(QProcess *process) {
  if (process == nullptr)
    return;
  if (process->state() != QProcess::NotRunning) {
    process->terminate();
    if (!process->waitForFinished(750)) {
      process->kill();
      process->waitForFinished(750);
    }
  }
  delete process;
}
} // namespace

RouterConnectionOptions
RouterConnectionOptions::fromConfig(const QString &applicationIniPath) {
  RouterConnectionOptions options;
  QSettings settings(applicationIniPath, QSettings::IniFormat);
  settings.beginGroup(QStringLiteral("Network"));

  const QString mode = settings.value(QStringLiteral("RouterMode"),
                                      QStringLiteral("local"))
                           .toString()
                           .trimmed()
                           .toLower();
  options.remote = mode == QStringLiteral("remote");
  options.sshConfigFile =
      settings.value(QStringLiteral("SSHConfigFile")).toString();
  options.sshProfile = settings.value(QStringLiteral("SSHProfile")).toString();
  options.remoteSamHost = settings.value(QStringLiteral("SSHRemoteSAMHost"),
                                         QStringLiteral("127.0.0.1"))
                              .toString();

  bool remotePortOk = false;
  const uint remotePort = settings.value(QStringLiteral("SSHRemoteSAMPort"), 7656)
                              .toUInt(&remotePortOk);
  if (!remotePortOk || remotePort < 1 || remotePort > 65535) {
    options.configurationError =
        QStringLiteral("The remote SAM port setting is invalid.");
  } else {
    options.remoteSamPort = static_cast<quint16>(remotePort);
  }

  bool forwardPortOk = false;
  const uint forwardPort = settings.value(QStringLiteral("SSHLocalForwardPort"), 0)
                               .toUInt(&forwardPortOk);
  if (!forwardPortOk || forwardPort > 65535) {
    options.configurationError =
        QStringLiteral("The local forwarding port setting is invalid.");
  } else {
    options.localForwardPort = static_cast<quint16>(forwardPort);
  }

  options.localSamHost = NetworkSettingsCompat::readSamHost(settings);
  const int localPort = NetworkSettingsCompat::readSamPort(settings);
  options.localSamPort = static_cast<quint16>(localPort);
  settings.endGroup();
  return options;
}

void RouterConnectionOptions::saveNetworkSettings(QSettings &settings) const {
  settings.setValue(QStringLiteral("RouterMode"),
                    remote ? QStringLiteral("remote") : QStringLiteral("local"));
  settings.setValue(QStringLiteral("SSHConfigFile"), sshConfigFile);
  settings.setValue(QStringLiteral("SSHProfile"), sshProfile);
  settings.setValue(QStringLiteral("SSHRemoteSAMHost"), remoteSamHost);
  settings.setValue(QStringLiteral("SSHRemoteSAMPort"), remoteSamPort);
  settings.setValue(QStringLiteral("SSHLocalForwardPort"), localForwardPort);
}

QString RouterConnectionOptions::validationError() const {
  if (!remote)
    return QString();
  if (!configurationError.isEmpty())
    return configurationError;
  if (sshConfigFile.trimmed().isEmpty())
    return QStringLiteral("Choose an SSH config file for Remote mode.");
  const QFileInfo configInfo(sshConfigFile);
  if (!configInfo.exists() || !configInfo.isFile() || !configInfo.isReadable())
    return QStringLiteral("The selected SSH config file is unavailable or unreadable.");
  if (!validProfile(sshProfile))
    return QStringLiteral("Enter a literal SSH Host alias using letters, numbers, '.', '_' or '-'.");
  if (!validSamHost(remoteSamHost))
    return QStringLiteral("Enter a valid remote SAM host name or IP address.");
  if (remoteSamPort == 0)
    return QStringLiteral("The remote SAM port must be between 1 and 65535.");
  return QString();
}

QStringList RouterConnectionOptions::sshProfiles(const QString &configPath,
                                                  QString *error) {
  if (error != nullptr)
    error->clear();
  QFile config(configPath);
  if (!config.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (error != nullptr)
      *error = QStringLiteral("Unable to read the selected SSH config file.");
    return {};
  }

  static const QRegularExpression hostLine(
      QStringLiteral("^\\s*Host\\s+(.+)$"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression literalHost(
      QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$"));
  QStringList profiles;
  bool inMatchBlock = false;
  while (!config.atEnd()) {
    QString line = QString::fromUtf8(config.readLine()).trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    const int comment = line.indexOf(QLatin1Char('#'));
    if (comment >= 0)
      line.truncate(comment);
    const QString lower = line.trimmed().toLower();
    if (lower.startsWith(QStringLiteral("match "))) {
      inMatchBlock = true;
      continue;
    }
    const QRegularExpressionMatch match = hostLine.match(line);
    if (!match.hasMatch())
      continue;
    inMatchBlock = false;
    const QStringList tokens = match.captured(1).split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
      if (token.startsWith(QLatin1Char('#')))
        break;
      if (!inMatchBlock && literalHost.match(token).hasMatch() &&
          !profiles.contains(token))
        profiles.append(token);
    }
  }
  return profiles;
}

QStringList RouterConnectionOptions::masterArguments(
    const QString &controlSocket, QString *error) const {
  if (error != nullptr)
    error->clear();
  const QString validation = validationError();
  if (!validation.isEmpty()) {
    if (error != nullptr)
      *error = validation;
    return {};
  }
  if (!remote) {
    if (error != nullptr)
      *error = QStringLiteral("SSH arguments are only available in Remote mode.");
    return {};
  }

  return {QStringLiteral("-M"),
          QStringLiteral("-N"),
          QStringLiteral("-T"),
          QStringLiteral("-S"),
          controlSocket,
          QStringLiteral("-F"),
          sshConfigFile,
          QStringLiteral("-o"),
          QStringLiteral("ControlMaster=yes"),
          QStringLiteral("-o"),
          QStringLiteral("ControlPersist=no"),
          QStringLiteral("-o"),
          QStringLiteral("ForkAfterAuthentication=no"),
          QStringLiteral("-o"),
          QStringLiteral("BatchMode=yes"),
          QStringLiteral("-o"),
          QStringLiteral("StrictHostKeyChecking=yes"),
          QStringLiteral("-o"),
          QStringLiteral("UpdateHostKeys=no"),
          QStringLiteral("-o"),
          QStringLiteral("PasswordAuthentication=no"),
          QStringLiteral("-o"),
          QStringLiteral("KbdInteractiveAuthentication=no"),
          QStringLiteral("-o"),
          QStringLiteral("PreferredAuthentications=publickey"),
          QStringLiteral("-o"),
          QStringLiteral("PubkeyAuthentication=yes"),
          QStringLiteral("-o"),
          QStringLiteral("ForwardAgent=no"),
          QStringLiteral("-o"),
          QStringLiteral("ForwardX11=no"),
          QStringLiteral("-o"),
          QStringLiteral("PermitLocalCommand=no"),
          QStringLiteral("-o"),
          QStringLiteral("LocalCommand=none"),
          QStringLiteral("-o"),
          QStringLiteral("RemoteCommand=none"),
          QStringLiteral("-o"),
          QStringLiteral("ClearAllForwardings=yes"),
          QStringLiteral("-o"),
          QStringLiteral("ExitOnForwardFailure=yes"),
          QStringLiteral("-o"),
          QStringLiteral("ConnectTimeout=10"),
          QStringLiteral("-o"),
          QStringLiteral("ServerAliveInterval=15"),
          QStringLiteral("-o"),
          QStringLiteral("ServerAliveCountMax=2"),
          sshProfile};
}

QStringList RouterConnectionOptions::forwardArguments(
    const QString &controlSocket, quint16 localPort, QString *error) const {
  if (error != nullptr)
    error->clear();
  const QString validation = validationError();
  if (!validation.isEmpty()) {
    if (error != nullptr)
      *error = validation;
    return {};
  }
  if (localPort == 0) {
    if (error != nullptr)
      *error = QStringLiteral("The local forwarding port has not been selected.");
    return {};
  }

  const QString forward =
      QStringLiteral("127.0.0.1:%1:%2:%3")
          .arg(localPort)
          .arg(optionHost(remoteSamHost))
          .arg(remoteSamPort);
  return {QStringLiteral("-F"),
          QStringLiteral("none"),
          QStringLiteral("-S"),
          controlSocket,
          QStringLiteral("-o"),
          QStringLiteral("ExitOnForwardFailure=yes"),
          QStringLiteral("-O"),
          QStringLiteral("forward"),
          QStringLiteral("-L"),
          forward,
          sshProfile};
}

RouterConnectionController::RouterConnectionController(QObject *parent)
    : QObject(parent) {
  mStartupTimer.setSingleShot(true);
  mProbeTimer.setSingleShot(true);
  mHealthTimer.setSingleShot(true);

  connect(&mStartupTimer, &QTimer::timeout, this, [this]() {
    if (!mFailureReported)
      handleStartupFailure(QStringLiteral("startup timeout"), false);
  });
  connect(&mProbeTimer, &QTimer::timeout, this, [this]() {
    if (mProbePending)
      handleProbeFailure(QStringLiteral("SAM HELLO timed out"));
  });
  connect(&mHealthTimer, &QTimer::timeout, this, [this]() {
    if (mMonitoring && mTunnelReady && !mProbePending)
      startProbe(mActiveHost, mActivePort, false, ProbePurpose::Periodic,
                 mGeneration);
  });
  connect(&mProbeSocket, &QTcpSocket::connected, this, [this]() {
    if (!mProbePending)
      return;
    mProbeSocket.write("HELLO VERSION MIN=3.1 MAX=3.1\n");
    mProbeSocket.flush();
  });
  connect(&mProbeSocket, &QTcpSocket::readyRead, this, [this]() {
    if (!mProbePending)
      return;
    mProbeReply.append(
        mProbeSocket.read(kSamReplyLimit + 1 - mProbeReply.size()));
    if (mProbeReply.size() > kSamReplyLimit) {
      handleProbeFailure(QStringLiteral("SAM HELLO response exceeded the size limit"));
      return;
    }
    const int newline = mProbeReply.indexOf('\n');
    if (newline < 0)
      return;
    QByteArray reply = mProbeReply.left(newline);
    if (reply.endsWith('\r'))
      reply.chop(1);
    if (mProbeReply.size() != newline + 1 ||
        reply != QByteArrayLiteral("HELLO REPLY RESULT=OK VERSION=3.1")) {
      handleProbeFailure(QStringLiteral("SAM HELLO response was invalid"));
      return;
    }
    handleProbeSuccess();
  });
  connect(&mProbeSocket, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
            if (mProbePending)
              handleProbeFailure(QStringLiteral("SAM endpoint did not answer"));
          });
  connect(&mProbeSocket, &QTcpSocket::disconnected, this, [this]() {
    if (mProbePending)
      handleProbeFailure(QStringLiteral("SAM endpoint closed before HELLO reply"));
  });
}

RouterConnectionController::~RouterConnectionController() { beginShutdown(); }

bool RouterConnectionController::isBusy() const {
  return mStartupTimer.isActive() || mProbePending ||
         mOperationProcess != nullptr;
}

bool RouterConnectionController::hasRemoteTunnel() const {
  return mTunnelReady && mMasterProcess != nullptr &&
         mMasterProcess->state() != QProcess::NotRunning;
}

QString RouterConnectionController::statusText() const { return mStatusText; }

void RouterConnectionController::setStatus(const QString &text) {
  if (mStatusText == text)
    return;
  mStatusText = text;
  emit signStatusChanged(text);
}

void RouterConnectionController::startRemote(
    const RouterConnectionOptions &options, bool keepAlive) {
  beginShutdown();
  mOptions = options;
  mOneShotHealth = !keepAlive;
  mFailureReported = false;
  mTunnelReady = false;
  mMonitoring = false;
  mMasterStderr.clear();
  mOperationStderr.clear();
  const quint64 generation = mGeneration;

  const QString validation = mOptions.validationError();
  if (!validation.isEmpty()) {
    handleStartupFailure(validation, true);
    return;
  }
#ifdef Q_OS_WIN
  handleStartupFailure(
      QStringLiteral("Remote SSH forwarding is not supported on this build."),
      true);
  return;
#endif
  QString portError;
  mSelectedLocalPort = reserveForwardPort(&portError);
  if (mSelectedLocalPort == 0) {
    handleStartupFailure(portError, true);
    return;
  }
  QString socketError;
  mControlSocket = createPrivateSocketPath(&socketError);
  if (mControlSocket.isEmpty()) {
    handleStartupFailure(socketError, true);
    return;
  }
  mActiveHost = QStringLiteral("127.0.0.1");
  mActivePort = mSelectedLocalPort;
  setStatus(QStringLiteral("Starting Remote SSH tunnel"));
  mStartupTimer.start(kSshStartupTimeoutMs);
  startMaster(generation);
}

void RouterConnectionController::checkLocal(const QString &host, quint16 port,
                                            const QString &applicationIniPath) {
  beginShutdown();
  mOneShotHealth = true;
  mFailureReported = false;
  mActiveHost = host;
  mActivePort = port;
  mOptions = RouterConnectionOptions();
  mApplicationIniPath = applicationIniPath;
  const quint64 generation = mGeneration;
  if (!validSamHost(host) || port == 0) {
    finishOneShotHealth(false, QStringLiteral("The active SAM endpoint is invalid."));
    return;
  }
  startProbe(host, port, true, ProbePurpose::HealthCheck, generation);
}

void RouterConnectionController::checkActiveRemote() {
  if (!hasRemoteTunnel()) {
    emit signHealthChecked(false,
                           QStringLiteral("No active Remote SSH tunnel is available."));
    return;
  }
  if (mProbePending)
    return;
  startProbe(mActiveHost, mActivePort, false, ProbePurpose::HealthCheck,
             mGeneration);
}

void RouterConnectionController::setOnlineMonitoring(bool enabled) {
  mMonitoring = enabled;
  mHealthTimer.stop();
  if (enabled && mTunnelReady)
    mHealthTimer.start(kHealthIntervalMs);
}

void RouterConnectionController::stop() {
  beginShutdown();
  mTunnelReady = false;
  mMonitoring = false;
  setStatus(QStringLiteral("Disconnected"));
}

void RouterConnectionController::startMaster(quint64 generation) {
  QString argumentError;
  const QStringList arguments =
      mOptions.masterArguments(mControlSocket, &argumentError);
  if (arguments.isEmpty()) {
    handleStartupFailure(argumentError, true);
    return;
  }
  mMasterProcess = new QProcess(this);
  QProcess *process = mMasterProcess;
  process->setProgram(processProgram(mOptions));
  process->setArguments(arguments);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  process->setStandardOutputFile(QProcess::nullDevice());
  connect(process, &QProcess::readyReadStandardError, this,
          [this, process, generation]() {
            if (generation == mGeneration && process == mMasterProcess)
              appendBounded(mMasterStderr, process->readAllStandardError());
          });
  connect(process, &QProcess::started, this, [this, process, generation]() {
    if (generation == mGeneration && process == mMasterProcess)
      checkMasterControlSocket(generation);
  });
  connect(process, &QProcess::errorOccurred, this,
          [this, process, generation](QProcess::ProcessError error) {
            if (generation != mGeneration || process != mMasterProcess ||
                mFailureReported)
              return;
            if (error == QProcess::FailedToStart) {
              mMasterProcess = nullptr;
              process->deleteLater();
              handleStartupFailure(
                  QStringLiteral("OpenSSH could not be started. Install OpenSSH or check its executable."),
                  true);
            }
          });
  connect(process,
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process, generation](int exitCode, QProcess::ExitStatus) {
            if (generation != mGeneration || process != mMasterProcess ||
                mExpectedStop || mFailureReported)
              return;
            appendBounded(mMasterStderr, process->readAllStandardError());
            mMasterProcess = nullptr;
            process->deleteLater();
            bool terminal = false;
            const QString message =
                classifySshFailure(QString::fromLocal8Bit(mMasterStderr),
                                   &terminal);
            if (mTunnelReady)
              handleTunnelLoss(message, terminal);
            else
              handleStartupFailure(
                  exitCode == 0 ? QStringLiteral("The SSH master exited unexpectedly.")
                                : message,
                  terminal);
          });
  process->start();
}

void RouterConnectionController::checkMasterControlSocket(quint64 generation) {
  if (generation != mGeneration || mFailureReported || mMasterProcess == nullptr)
    return;
  if (mOperationProcess != nullptr)
    return;
  startControlOperation(
      ControlOperation::CheckMaster,
      {QStringLiteral("-F"), QStringLiteral("none"), QStringLiteral("-S"),
       mControlSocket, QStringLiteral("-O"), QStringLiteral("check"),
       mOptions.sshProfile},
      generation);
}

void RouterConnectionController::startControlOperation(
    ControlOperation operation, const QStringList &arguments,
    quint64 generation) {
  if (mOperationProcess != nullptr || generation != mGeneration)
    return;
  mOperationStderr.clear();
  mControlOperation = operation;
  mOperationProcess = new QProcess(this);
  QProcess *process = mOperationProcess;
  process->setProgram(processProgram(mOptions));
  process->setArguments(arguments);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  process->setStandardOutputFile(QProcess::nullDevice());
  connect(process, &QProcess::readyReadStandardError, this,
          [this, process, generation]() {
            if (generation == mGeneration && process == mOperationProcess)
              appendBounded(mOperationStderr, process->readAllStandardError());
          });
  connect(process,
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process, generation](int exitCode,
                                      QProcess::ExitStatus status) {
            if (generation != mGeneration || process != mOperationProcess)
              return;
            appendBounded(mOperationStderr, process->readAllStandardError());
            mOperationProcess = nullptr;
            process->deleteLater();
            handleControlOperationFinished(exitCode, status, generation);
          });
  connect(process, &QProcess::errorOccurred, this,
          [this, process, generation](QProcess::ProcessError error) {
            if (generation != mGeneration || process != mOperationProcess)
              return;
            if (error == QProcess::FailedToStart) {
              mOperationProcess = nullptr;
              process->deleteLater();
              if (mControlOperation == ControlOperation::CheckMaster) {
                handleStartupFailure(
                    QStringLiteral("OpenSSH could not be started."), true);
              } else {
                handleControlOperationFinished(-1, QProcess::CrashExit,
                                               generation);
              }
            }
          });
  process->start();
}

void RouterConnectionController::handleControlOperationFinished(
    int exitCode, QProcess::ExitStatus, quint64 generation) {
  const ControlOperation operation = mControlOperation;
  mControlOperation = ControlOperation::None;
  if (generation != mGeneration || mFailureReported)
    return;
  if (operation == ControlOperation::CheckMaster) {
    if (exitCode == 0) {
      startForward(generation);
      return;
    }
    const QString controlError = QString::fromLocal8Bit(mOperationStderr);
    const QString lowerError = controlError.toLower();
    if (lowerError.contains(QStringLiteral("bad configuration option")) ||
        lowerError.contains(QStringLiteral("unknown configuration option")) ||
        lowerError.contains(QStringLiteral("unsupported option")) ||
        lowerError.contains(QStringLiteral("unknown option")) ||
        lowerError.contains(QStringLiteral("illegal option")) ||
        lowerError.contains(QStringLiteral("operation not supported"))) {
      bool terminal = false;
      const QString message = classifySshFailure(controlError, &terminal);
      handleStartupFailure(message, terminal);
      return;
    }
    QTimer::singleShot(kControlCheckIntervalMs, this, [this, generation]() {
      checkMasterControlSocket(generation);
    });
    return;
  }
  if (operation == ControlOperation::AddForward) {
    if (exitCode != 0) {
      bool terminal = false;
      const QString message = classifySshFailure(
          QString::fromLocal8Bit(mOperationStderr), &terminal);
      handleStartupFailure(message, terminal);
      return;
    }
    mStartupTimer.stop();
    mTunnelReady = true;
    mActiveHost = QStringLiteral("127.0.0.1");
    mActivePort = mSelectedLocalPort;
    startProbe(mActiveHost, mActivePort, false, ProbePurpose::Startup,
               generation);
  }
}


void RouterConnectionController::startForward(quint64 generation) {
  QString argumentError;
  const QStringList arguments = mOptions.forwardArguments(
      mControlSocket, mSelectedLocalPort, &argumentError);
  if (arguments.isEmpty()) {
    handleStartupFailure(argumentError, true);
    return;
  }
  setStatus(QStringLiteral("Confirming loopback SAM forwarding"));
  startControlOperation(ControlOperation::AddForward, arguments, generation);
}

void RouterConnectionController::startProbe(const QString &host, quint16 port,
                                            bool useLegacyProxy,
                                            ProbePurpose purpose,
                                            quint64 generation) {
  if (generation != mGeneration || mProbePending)
    return;
  mProbePending = false;
  mProbeTimer.stop();
  mProbeSocket.abort();
  mProbePurpose = purpose;
  mProbeReply.clear();
  mProbeSocket.setProxy(QNetworkProxy::NoProxy);
  mProbeSocket.setReadBufferSize(kSamReplyLimit + 1);
  if (useLegacyProxy && !mApplicationIniPath.isEmpty()) {
    QSettings settings(mApplicationIniPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Network"));
    if (settings.value(QStringLiteral("SOCKS5ProxyEnabled"), false).toBool()) {
      const QString proxyHost =
          settings.value(QStringLiteral("SOCKS5ProxyHost")).toString();
      const int proxyPort = settings.value(QStringLiteral("SOCKS5ProxyPort"), 1080)
                                .toInt();
      if (!proxyHost.isEmpty() && proxyPort > 0 && proxyPort <= 65535) {
        QNetworkProxy proxy(QNetworkProxy::Socks5Proxy, proxyHost,
                            static_cast<quint16>(proxyPort),
                            settings.value(QStringLiteral("SOCKS5ProxyUsername"))
                                .toString(),
                            settings.value(QStringLiteral("SOCKS5ProxyPassword"))
                                .toString());
        mProbeSocket.setProxy(proxy);
      }
    }
    settings.endGroup();
  }
  mProbePending = true;
  setStatus(purpose == ProbePurpose::Periodic
                ? QStringLiteral("Checking Remote SAM health")
                : QStringLiteral("Checking SAM HELLO"));
  mProbeTimer.start(kSamProbeTimeoutMs);
  mProbeSocket.connectToHost(host, port);
}

void RouterConnectionController::handleProbeFailure(const QString &reason,
                                                    bool terminal) {
  if (!mProbePending)
    return;
  mProbePending = false;
  mProbeTimer.stop();
  mProbeSocket.abort();
  const QString text = samFailureText(reason);
  if (mProbePurpose == ProbePurpose::Periodic) {
    handleTunnelLoss(text, terminal);
    return;
  }
  if (mProbePurpose == ProbePurpose::HealthCheck) {
    if (mOneShotHealth) {
      finishOneShotHealth(false, text);
    } else {
      emit signHealthChecked(false, text);
      if (mMonitoring)
        handleTunnelLoss(text, terminal);
      else
        setStatus(text);
    }
    return;
  }
  handleStartupFailure(text, terminal);
}

void RouterConnectionController::handleProbeSuccess() {
  mProbePending = false;
  mProbeTimer.stop();
  mProbeSocket.disconnectFromHost();
  if (mProbePurpose == ProbePurpose::HealthCheck) {
    const QString text = QStringLiteral("SAM HELLO 3.1 succeeded.");
    if (mOneShotHealth) {
      finishOneShotHealth(true, QStringLiteral("SAM HELLO 3.1 succeeded."));
    } else {
      emit signHealthChecked(true, text);
      setStatus(text);
      if (mMonitoring)
        mHealthTimer.start(kHealthIntervalMs);
    }
    return;
  }
  if (mProbePurpose == ProbePurpose::Periodic) {
    setStatus(QStringLiteral("Remote SAM is healthy"));
    mHealthTimer.start(kHealthIntervalMs);
    return;
  }
  if (mOneShotHealth) {
    finishOneShotHealth(true,
                         QStringLiteral("SAM HELLO succeeded; temporary tunnel closed."));
    return;
  }
  setStatus(QStringLiteral("SAM HELLO 3.1 succeeded; starting SAM session"));
  emit signReady(mActiveHost, mActivePort);
  if (mMonitoring)
    mHealthTimer.start(kHealthIntervalMs);
}

void RouterConnectionController::handleStartupFailure(const QString &reason,
                                                       bool terminal) {
  if (mFailureReported)
    return;
  mFailureReported = true;
  mStartupTimer.stop();
  mProbeTimer.stop();
  const QString text = reason.isEmpty() ? QStringLiteral("Remote SSH connection failed.")
                                        : reason;
  beginShutdown();
  setStatus(text);
  if (mOneShotHealth)
    emit signHealthChecked(false, text);
  emit signFailed(text, terminal);
}

void RouterConnectionController::handleTunnelLoss(const QString &reason,
                                                   bool terminal) {
  if (mFailureReported)
    return;
  mFailureReported = true;
  const QString text = reason.isEmpty() ? QStringLiteral("Remote connection was lost.")
                                        : reason;
  beginShutdown();
  mTunnelReady = false;
  mMonitoring = false;
  setStatus(text);
  if (mOneShotHealth) {
    emit signHealthChecked(false, text);
    return;
  }
  emit signConnectionLost(text, terminal);
}

void RouterConnectionController::finishOneShotHealth(bool success,
                                                      const QString &text) {
  beginShutdown();
  mTunnelReady = false;
  mMonitoring = false;
  setStatus(text);
  emit signHealthChecked(success, text);
}

void RouterConnectionController::beginShutdown() {
  mExpectedStop = true;
  ++mGeneration;
  mStartupTimer.stop();
  mProbeTimer.stop();
  mHealthTimer.stop();
  mProbePending = false;
  mProbeSocket.abort();
  mControlOperation = ControlOperation::None;
  QProcess *operation = mOperationProcess;
  mOperationProcess = nullptr;
  QProcess *master = mMasterProcess;
  mMasterProcess = nullptr;
  stopOwnedProcess(operation);
  stopOwnedProcess(master);
  if (!mPrivateDirectory.isEmpty()) {
    QDir directory(mPrivateDirectory);
    directory.removeRecursively();
    mPrivateDirectory.clear();
  }
  mControlSocket.clear();
  mSelectedLocalPort = 0;
  mExpectedStop = false;
}

QString RouterConnectionController::createPrivateSocketPath(QString *error) {
  QString base = QStringLiteral("/tmp");
  if (!QFileInfo(base).isDir())
    base = QDir::tempPath();

  QTemporaryDir directory(base + QStringLiteral("/i2pc-XXXXXX"));
  if (!directory.isValid()) {
    if (error != nullptr)
      *error = QStringLiteral("Could not create a private short SSH control-socket path.");
    return QString();
  }
  directory.setAutoRemove(false);

  const QString path = directory.path();
  const QFileDevice::Permissions permissions =
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
  if (!QFile::setPermissions(path, permissions)) {
    QDir(path).removeRecursively();
    if (error != nullptr)
      *error = QStringLiteral("Could not secure the private SSH control-socket directory.");
    return QString();
  }

  const QString socket = path + QStringLiteral("/s");
  if (socket.toLocal8Bit().size() > 90) {
    QDir(path).removeRecursively();
    if (error != nullptr)
      *error = QStringLiteral("Could not create a short SSH control-socket path.");
    return QString();
  }

  mPrivateDirectory = path;
  return socket;
}

quint16 RouterConnectionController::reserveForwardPort(QString *error) const {
  QTcpServer server;
  const QHostAddress loopback(QStringLiteral("127.0.0.1"));
  if (mOptions.localForwardPort == 0) {
    if (!server.listen(loopback, 0)) {
      if (error != nullptr)
        *error = QStringLiteral("Could not reserve an automatic loopback forwarding port.");
      return 0;
    }
    const quint16 port = server.serverPort();
    server.close();
    return port;
  }
  if (!server.listen(loopback, mOptions.localForwardPort)) {
    if (error != nullptr)
      *error = QStringLiteral("The selected local forwarding port is already in use.");
    return 0;
  }
  const quint16 port = server.serverPort();
  server.close();
  return port;
}

QString RouterConnectionController::classifySshFailure(
    const QString &stderrText, bool *terminal) {
  const QString lower = stderrText.toLower();
  bool isTerminal = false;
  QString message = QStringLiteral("SSH connection failed.");
  if (lower.contains(QStringLiteral("permission denied")) ||
      lower.contains(QStringLiteral("authentication failed"))) {
    isTerminal = true;
    message = QStringLiteral("SSH public-key authentication failed. Check the key or ssh-agent.");
  } else if (lower.contains(QStringLiteral("host key verification failed")) ||
             lower.contains(QStringLiteral("remote host identification has changed"))) {
    isTerminal = true;
    message = QStringLiteral("SSH host-key verification failed. Confirm the host key in a trusted terminal.");
  } else if (lower.contains(QStringLiteral("address already in use")) ||
             lower.contains(QStringLiteral("cannot listen to port")) ||
             lower.contains(QStringLiteral("cannot listen on port"))) {
    isTerminal = true;
    message = QStringLiteral("The selected local forwarding port is already in use.");
  } else if (lower.contains(QStringLiteral("bad configuration option")) ||
             lower.contains(QStringLiteral("unknown configuration option")) ||
             lower.contains(QStringLiteral("unsupported option")) ||
             lower.contains(QStringLiteral("unknown option")) ||
             lower.contains(QStringLiteral("illegal option")) ||
             (lower.contains(QStringLiteral("controlmaster")) &&
              lower.contains(QStringLiteral("unsupported"))) ||
             (lower.contains(QStringLiteral("multiplexing")) &&
              lower.contains(QStringLiteral("not supported"))) ||
             lower.contains(QStringLiteral("operation not supported"))) {
    isTerminal = true;
    message = QStringLiteral("This OpenSSH build does not support the required ControlMaster forwarding options.");
  } else if (lower.contains(QStringLiteral("could not resolve hostname")) ||
             lower.contains(QStringLiteral("no such file or directory")) ||
             lower.contains(QStringLiteral("bad owner or permissions")) ||
             lower.contains(QStringLiteral("administratively prohibited"))) {
    isTerminal = true;
    message = QStringLiteral("SSH configuration, profile, or forwarding policy was rejected.");
  }
  if (terminal != nullptr)
    *terminal = isTerminal;
  return message;
}

QString RouterConnectionController::samFailureText(const QString &reason) {
  if (reason.contains(QStringLiteral("size limit")))
    return QStringLiteral("SAM HELLO response exceeded the allowed size.");
  if (reason.contains(QStringLiteral("invalid")))
    return QStringLiteral("The endpoint did not return the required SAM 3.1 HELLO reply.");
  if (reason.contains(QStringLiteral("timed out")))
    return QStringLiteral("SAM HELLO timed out.");
  return QStringLiteral("Could not complete SAM HELLO with the configured endpoint.");
}

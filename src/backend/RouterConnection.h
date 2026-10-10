#ifndef ROUTERCONNECTION_H
#define ROUTERCONNECTION_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

class QSettings;

struct RouterConnectionOptions {
  bool remote = false;
  QString sshConfigFile;
  QString sshProfile;
  QString remoteSamHost = QStringLiteral("127.0.0.1");
  quint16 remoteSamPort = 7656;
  quint16 localForwardPort = 0;
  QString localSamHost = QStringLiteral("127.0.0.1");
  quint16 localSamPort = 7656;
  QString configurationError;

  // Runtime-only executable override used by the standalone controller tests.
  // It is never read from or written to application settings.
  QString sshExecutable;

  static RouterConnectionOptions fromConfig(const QString &applicationIniPath);
  void saveNetworkSettings(QSettings &settings) const;
  QString validationError() const;
  static QStringList sshProfiles(const QString &configPath,
                                 QString *error = nullptr);

  QStringList masterArguments(const QString &controlSocket,
                              QString *error = nullptr) const;
  QStringList forwardArguments(const QString &controlSocket,
                               quint16 localPort,
                               QString *error = nullptr) const;
};

class RouterConnectionController : public QObject {
  Q_OBJECT
public:
  explicit RouterConnectionController(QObject *parent = nullptr);
  ~RouterConnectionController() override;

  bool isBusy() const;
  bool hasRemoteTunnel() const;
  QString statusText() const;

  // Starts a remote tunnel for an Online attempt or an offline, one-shot
  // health check. No application session is created by this controller.
  void startRemote(const RouterConnectionOptions &options, bool keepAlive);
  void checkLocal(const QString &host, quint16 port,
                  const QString &applicationIniPath);
  void checkActiveRemote();
  void setOnlineMonitoring(bool enabled);
  void stop();

signals:
  void signStatusChanged(const QString &text);
  void signReady(const QString &host, quint16 port);
  void signFailed(const QString &text, bool terminal);
  void signHealthChecked(bool success, const QString &text);
  void signConnectionLost(const QString &text, bool terminal);

private:
  enum class ControlOperation { None, CheckMaster, AddForward };
  enum class ProbePurpose { Startup, HealthCheck, Periodic };

  void setStatus(const QString &text);
  void startMaster(quint64 generation);
  void checkMasterControlSocket(quint64 generation);
  void startControlOperation(ControlOperation operation,
                             const QStringList &arguments,
                             quint64 generation);
  void handleControlOperationFinished(int exitCode, QProcess::ExitStatus status,
                                      quint64 generation);
  void startForward(quint64 generation);
  void startProbe(const QString &host, quint16 port, bool useLegacyProxy,
                  ProbePurpose purpose, quint64 generation);
  void handleProbeFailure(const QString &reason, bool terminal = false);
  void handleProbeSuccess();
  void handleStartupFailure(const QString &reason, bool terminal);
  void handleTunnelLoss(const QString &reason, bool terminal);
  void finishOneShotHealth(bool success, const QString &text);
  void beginShutdown();
  QString createPrivateSocketPath(QString *error);
  quint16 reserveForwardPort(QString *error) const;
  static QString classifySshFailure(const QString &stderrText,
                                    bool *terminal = nullptr);
  static QString samFailureText(const QString &reason);

  RouterConnectionOptions mOptions;
  QString mApplicationIniPath;
  QProcess *mMasterProcess = nullptr;
  QProcess *mOperationProcess = nullptr;
  QTcpSocket mProbeSocket;
  QTimer mStartupTimer;
  QTimer mProbeTimer;
  QTimer mHealthTimer;
  QByteArray mProbeReply;
  QByteArray mMasterStderr;
  QByteArray mOperationStderr;
  QString mPrivateDirectory;
  QString mControlSocket;
  QString mStatusText;
  QString mActiveHost;
  quint16 mActivePort = 0;
  quint16 mSelectedLocalPort = 0;
  quint64 mGeneration = 0;
  ControlOperation mControlOperation = ControlOperation::None;
  ProbePurpose mProbePurpose = ProbePurpose::Startup;
  bool mTunnelReady = false;
  bool mMonitoring = false;
  bool mProbePending = false;
  bool mOneShotHealth = false;
  bool mExpectedStop = false;
  bool mFailureReported = false;
};

#endif // ROUTERCONNECTION_H

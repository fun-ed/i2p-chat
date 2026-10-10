#ifndef NETWORKSETTINGSCOMPAT_H
#define NETWORKSETTINGSCOMPAT_H

#include <QSettings>
#include <QString>

namespace NetworkSettingsCompat {

QString readSamHost(const QSettings &settings);
int readSamPort(const QSettings &settings);
QString readSignatureType(const QSettings &settings);
int readSamConnectTimeoutMs(const QSettings &settings);

void writeSamHost(QSettings &settings, const QString &host);
void writeSamPort(QSettings &settings, int port);
void writeSignatureType(QSettings &settings, const QString &signatureType);
void migrateNetworkCompatibilityKeys(QSettings &settings);

} // namespace NetworkSettingsCompat

#endif // NETWORKSETTINGSCOMPAT_H

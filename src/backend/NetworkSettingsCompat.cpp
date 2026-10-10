#include "NetworkSettingsCompat.h"

#include <QVariant>

namespace {

QString readStringWithLegacy(const QSettings &settings, const QString &canonical,
                             const QStringList &legacyKeys,
                             const QString &defaultValue) {
  if (settings.contains(canonical)) {
    return settings.value(canonical, defaultValue).toString();
  }

  for (const QString &legacyKey : legacyKeys) {
    if (settings.contains(legacyKey)) {
      return settings.value(legacyKey, defaultValue).toString();
    }
  }

  return defaultValue;
}

int readIntWithLegacy(const QSettings &settings, const QString &canonical,
                      const QStringList &legacyKeys, const int defaultValue,
                      const int minValue, const int maxValue) {
  bool ok = false;
  int value = readStringWithLegacy(settings, canonical, legacyKeys,
                                   QString::number(defaultValue))
                  .toInt(&ok);
  if (!ok) {
    value = defaultValue;
  }

  if (value < minValue) {
    value = minValue;
  }
  if (value > maxValue) {
    value = maxValue;
  }

  return value;
}

void writeCanonicalWithCleanup(QSettings &settings, const QString &canonical,
                               const QVariant &value,
                               const QStringList &legacyKeys) {
  settings.setValue(canonical, value);
  for (const QString &legacyKey : legacyKeys) {
    if (settings.contains(legacyKey)) {
      settings.remove(legacyKey);
    }
  }
}

} // namespace

namespace NetworkSettingsCompat {

QString readSamHost(const QSettings &settings) {
  return readStringWithLegacy(settings, "SAMHost", {"SamHost"}, "127.0.0.1");
}

int readSamPort(const QSettings &settings) {
  return readIntWithLegacy(settings, "SAMPort", {"SamPort"}, 7656, 1, 65535);
}

QString readSignatureType(const QSettings &settings) {
  return readStringWithLegacy(settings, "SignatureType",
                              {"Signature_Type", "SIGNATURE_TYPE"},
                              "EdDSA_SHA512_Ed25519");
}

int readSamConnectTimeoutMs(const QSettings &settings) {
  return readIntWithLegacy(settings, "SamConnectTimeoutMs",
                           {"SAMConnectTimeoutMs", "SamConnectTimeout"},
                           5000, 1000, 60000);
}

void writeSamHost(QSettings &settings, const QString &host) {
  const QString normalizedHost = host.trimmed().isEmpty() ? "127.0.0.1" : host;
  writeCanonicalWithCleanup(settings, "SAMHost", normalizedHost, {"SamHost"});
}

void writeSamPort(QSettings &settings, int port) {
  if (port < 1) {
    port = 1;
  }
  if (port > 65535) {
    port = 65535;
  }
  writeCanonicalWithCleanup(settings, "SAMPort", port, {"SamPort"});
}

void writeSignatureType(QSettings &settings, const QString &signatureType) {
  const QString normalizedSignature =
      signatureType.trimmed().isEmpty() ? "EdDSA_SHA512_Ed25519"
                                        : signatureType.trimmed();
  writeCanonicalWithCleanup(settings, "SignatureType", normalizedSignature,
                            {"Signature_Type", "SIGNATURE_TYPE"});
}

void migrateNetworkCompatibilityKeys(QSettings &settings) {
  writeSamHost(settings, readSamHost(settings));
  writeSamPort(settings, readSamPort(settings));
  writeSignatureType(settings, readSignatureType(settings));
  settings.setValue("SamConnectTimeoutMs", readSamConnectTimeoutMs(settings));
}

} // namespace NetworkSettingsCompat

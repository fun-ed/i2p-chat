#include "I2PSamMessageAnalyser.h"

#include <QCoreApplication>
#include <iostream>

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  CI2PSamMessageAnalyser analyser("sam-cpp-smoke");
  using namespace SAM_Message_Types;

  struct Case {
    const char *line;
    TYPE type;
    RESULT result;
  };
  const Case cases[] = {
      {"HELLO REPLY RESULT=OK VERSION=3.1\n", HELLO_REPLAY, OK},
      {"HELLO REPLY RESULT=NOVERSION\n", HELLO_REPLAY, NOVERSION},
      {"SESSION STATUS RESULT=OK\n", SESSION_STATUS, OK},
      {"SESSION STATUS RESULT=DUPLICATED_ID\n", SESSION_STATUS, DUPLICATED_ID},
      {"SESSION STATUS RESULT=INVALID_KEY\n", SESSION_STATUS, INVALID_KEY},
      {"STREAM STATUS RESULT=OK\n", STREAM_STATUS, OK},
      {"STREAM STATUS RESULT=CANT_REACH_PEER\n", STREAM_STATUS, CANT_REACH_PEER},
      {"STREAM STATUS RESULT=TIMEOUT\n", STREAM_STATUS, TIMEOUT},
      {"NAMING REPLY RESULT=KEY_NOT_FOUND NAME=missing.i2p\n", NAMING_REPLY,
       KEY_NOT_FOUND},
      {"", ERROR_IN_ANALYSE, FAILED},
      {"HELLO", ERROR_IN_ANALYSE, FAILED},
      {"HELLO REPLY", HELLO_REPLAY, FAILED},
      {"SESSION STATUS", SESSION_STATUS, FAILED},
      {"STREAM STATUS", STREAM_STATUS, FAILED},
      {"NAMING REPLY", NAMING_REPLY, FAILED},
      {"UNKNOWN REPLY RESULT=OK\n", ERROR_IN_ANALYSE, FAILED},
      {"STREAM STATUS RESULT=UNEXPECTED\n", ERROR_IN_ANALYSE, FAILED},
  };

  int checked = 0;
  for (const auto &test : cases) {
    const auto message = analyser.Analyse(QString::fromUtf8(test.line));
    if (message.type != test.type || message.result != test.result) {
      std::cerr << "FAIL: " << test.line << '\n';
      return 1;
    }
    ++checked;
  }

  const auto naming = analyser.Analyse(
      "NAMING REPLY RESULT=OK NAME=ME VALUE=public-destination\n");
  if (naming.type != NAMING_REPLY || naming.result != OK ||
      naming.Name != "ME" || naming.Value != "public-destination") {
    std::cerr << "FAIL: naming fields\n";
    return 1;
  }
  ++checked;

  const auto destination =
      analyser.Analyse("DEST REPLY PUB=public-key PRIV=private-key\n");
  if (destination.type != DEST_REPLY || destination.PUB != "public-key" ||
      destination.PRIV != "private-key") {
    std::cerr << "FAIL: destination fields\n";
    return 1;
  }
  ++checked;

  std::cout << "PASS: " << checked << " C++ SAM parser cases\n";
  return 0;
}

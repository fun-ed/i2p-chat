# Repository Guidelines

## Project Overview

I2PChat is a Qt Widgets peer-to-peer messenger over I2P, with contacts, profiles, offline message storage, and file transfers. The C++ client uses SAM with an external local Java I2P router or a remote SAM-compatible router such as i2pd through managed SSH forwarding. The reference local runtime is Java I2P 2.13.0 on Temurin JDK 25 LTS; there is no Rust integration.

## Architecture & Data Flow

- `src/backend/Main.cpp` starts `QApplication` and `form_MainWindow`. The window constructs `CCore`, which wires the connection, user, protocol, file-transfer, sound, and storage managers.
- `CConnectionManager` manages `CSessionController` for SAM control commands and `CI2PStream` objects for peer connections. Socket callbacks buffer newline-delimited SAM replies, then `CI2PSamMessageAnalyser` parses them.
- `RouterConnectionController` owns Remote OpenSSH processes, a private ControlMaster socket, forwarding acknowledgement, and SAM HELLO probes. Core gates Remote session creation on this readiness and keeps desired presence separate from Connecting. Preserve cancellation generations, stopped-only endpoint changes, and the finite reconnect budget.
- Remote session and peer streams use the same loopback endpoint with `NoProxy`; do not apply saved SOCKS5 settings to the SSH-forwarded route. Offline health checks use temporary tunnels without creating application sessions.
- Peer streams start as `UNKNOWN`. `CProtocol` recognizes `CHATSYSTEM\t0.6\n`, associates the stream with a user through Core, and switches it to `KNOWN`. `CPacketManager` assembles chat packets from arbitrary socket chunks before protocol dispatch. Framing is four hex length bytes, four info bytes, then payload.
- File transfers use separate streams and the `CHATSYSTEMFILETRANSFER` handshake. Preserve their separation from framed chat traffic.

## Key Directories

- `src/backend/` contains application coordination, SAM networking, protocol, persistence, and transfer logic.
- `src/gui/` contains Qt Widgets implementations, Designer `.ui` forms, icons, and `resourcen.qrc`.
- `tools/` contains SAM parser smoke tests, Remote controller/Core/GUI Qt tests with local fixtures, and the pinned Java I2P runtime verifier.
- `qss/`, `sounds/`, and `templates/` contain presentation assets. `docs/` contains dated research; `plan.md` records the current Java I2P decision and verification status.
- `temp/{obj,qrc,moc}/` and `src/gui/ui_*.h` are generated build outputs. Edit `.ui` forms rather than generated headers.

## Development Commands

Run from the repository root with the intended Qt installation's `qmake` on `PATH`:

```sh
# Release build
qmake I2PChat.pro "CONFIG += release"
make -j8

# Debug build
qmake I2PChat.pro "CONFIG += debug"
make -j8

# C++ SAM parser smoke, no live router needed
mkdir -p build/sam-cpp-smoke
qmake tools/sam_cpp_smoke.pro -o build/sam-cpp-smoke/Makefile
make -C build/sam-cpp-smoke -j8
./build/sam-cpp-smoke/sam_cpp_smoke

# SSH/SAM controller, Core and settings tests; temporary local fixtures only
mkdir -p build/remote-connection-test
qmake tools/remote_connection_test.pro -o build/remote-connection-test/Makefile
make -C build/remote-connection-test -j8
QT_QPA_PLATFORM=offscreen ./build/remote-connection-test/remote_connection_test

# Project-local JDK installation and Java I2P artifact/version check
mise install
mise run verify-java-i2p

# Run on Linux
./I2PChat --profile /tmp/i2pchat-dev-profile
# Run on macOS
./I2PChat.app/Contents/MacOS/I2PChat --profile /tmp/i2pchat-dev-profile
```

There is no configured repository-wide lint or formatter target. The C++ build enables `-Wall -Wextra`; preserve nearby formatting rather than reformatting entire files.

## Code Conventions & Common Patterns

- Backend classes commonly use a `C` prefix, members an `m` prefix, methods camelCase, signals `sign...`, and slots `slot...`. Legacy naming and indentation vary; follow the surrounding file.
- Use existing Qt types and APIs such as `QString`, `QByteArray`, `QMap`, and `QSettings`. Most connections use string-based `SIGNAL`/`SLOT` syntax.
- Async work uses `QTcpSocket`, Qt signals/slots, and `QTimer` in the event loop. Some connection paths still call `waitForConnected`; do not assume a worker-thread architecture or fully nonblocking behavior.
- Core constructs managers and passes `CCore` or manager references where needed. Dependencies are wired directly, without a dependency-injection framework. State lives in managers, stream registries, enums, and handshake flags.
- Ownership mixes explicit raw-pointer cleanup and QObject lifetimes. Inspect destructors, registry removal, and `deleteLater()` before changing object lifetime; parent ownership is not universal.
- Network errors generally use SAM result/status enums, debug signals, and Qt logging. Some legacy paths use dialogs or exceptions; match the affected path's handling.

## Important Files

- `I2PChat.pro` registers sources, headers, forms, resources, and platform build settings; update it when adding application files. `gitversion.pri` derives version metadata from Git.
- `Core.cpp`, `RouterConnection.cpp`, `ConnectionManager.cpp`, `SessionController.cpp`, and `I2PStream.cpp` under `src/backend/` define lifecycle and connection behavior. `Protocol.cpp` and `PacketManager.cpp` define chat routing and framing.
- `application.ini.example` documents runtime settings. Use `src/backend/NetworkSettingsCompat.cpp` for canonical SAM keys and legacy-key migration instead of duplicating settings readers.
- `mise.toml` pins the Java runtime and I2P verification baseline. Before changing router/runtime versions, read `plan.md`, update the verifier's artifact SHA-256 values, and rerun the runtime check. The research report is a dated snapshot, not an active migration plan.

## Runtime/Tooling Preferences

- Current build definitions select C++17, require Qt 5.15+, and use qmake/make. Qt modules are Core, Network, XML, Widgets, Multimedia, and SVG. Older prose claiming mandatory C++20 or Qt 6.4+ is not authoritative.
- macOS currently targets 15.0; universal architectures are commented out in `I2PChat.pro`. Homebrew is used by the macOS setup helper. The external router uses project-pinned Temurin JDK 25 LTS through mise; the C++ build does not require Java. No Cargo, Node, or Bun workflow is required for the main app.
- Default desktop configuration on Linux/macOS is `~/.i2pchat/application.ini`; Windows uses Qt's `AppDataLocation`. `--profile <directory>` overrides the desktop profile directory. Use disposable profiles for manual testing.
- SAM defaults to `127.0.0.1:7656`. Configure SOCKS5 through `[Network] SOCKS5ProxyEnabled` and its host/port/auth keys, rather than relying on older `ConnectionMode` examples.
- `RouterMode=local|remote` is distinct from legacy `ConnectionMode`. Remote uses `SSHConfigFile`, `SSHProfile` (literal Host alias), `SSHRemoteSAMHost`, `SSHRemoteSAMPort`, and `SSHLocalForwardPort` (0 = automatic). Route edits require Offline; active checks reuse the current route.
- Remote requires system OpenSSH with ControlMaster support (macOS/Linux; Windows Remote is rejected). It uses key/agent authentication and pre-trusted host keys; never relax host-key checks or log raw SSH stderr. Selecting config does not authorize an assistant to connect to a real VPS during development.
- Enable SAM separately in Java I2P. `mise run verify-java-i2p` checks pinned artifact hashes and version entry points without starting a router; it does not validate live SAM, tunnels, peers, chat, or transfers. Avoid running setup scripts against existing router services without reviewing their side effects.

## Testing & QA

- Build and run `tools/sam_cpp_smoke.pro` for SAM parser changes. It covers successful replies, naming/key fields, errors, unknown input, and truncated messages without Rust or a live router.
- `tools/remote_connection_test.pro` uses QtTest separately from the app and covers local fake SSH/SAM/SOCKS fixtures, Core readiness/retries/cancellation, and settings-state protection. Keep tests off real routers and private SSH files. No repository-wide coverage threshold is configured.
- Ordinary C++/GUI changes need a qmake build and focused checks with a disposable profile. SAM HELLO or a build does not prove live I2P connectivity; chat and transfer checks require a configured router and peers.
- `setup_macos.sh` installs dependencies and can change router configuration/services. `test_socks5_setup.sh` overwrites user configuration; `test_my_setup.sh` uses machine-specific SSH/network settings and can alter configuration/logs. Inspect these before running; they are not routine automated tests.

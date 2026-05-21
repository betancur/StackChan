# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

StackChan is a multi-component monorepo for an open-source AI desktop robot built on M5Stack CoreS3 (ESP32-S3). It has four components that communicate with each other:

- **`app/`** — Flutter mobile app (iOS/Android) — connects to the robot via BLE and to the server via HTTP/WebSocket
- **`server/`** — Go backend (GoFrame v2) — REST API + WebSocket broker, backed by MySQL
- **`firmware/`** — ESP-IDF v5.5.4 C++ firmware — runs on the CoreS3 hardware
- **`remote/`** — ESP-IDF v5.4.2 Arduino firmware — StickC-Plus remote controller via ESP-NOW

## Commands

### Flutter App (`app/`)

```bash
flutter pub get           # install dependencies
flutter analyze           # lint (flutter_lints configured)
flutter test              # run all tests
flutter test test/widget_test.dart  # run single test file
flutter run -d ios        # run on iOS
flutter run -d android    # run on Android
flutter build ios --release
flutter build apk --release
flutter build appbundle --release
```

Before first iOS build: `cd ios && pod install && cd ..`

### Go Server (`server/`)

```bash
go mod download           # install dependencies
go run main.go            # run in dev mode
go build -o stackchan-server main.go  # build binary
make build                # build via Makefile
make run                  # run via Makefile
```

Server listens on port **12800**. Config is at `manifest/config/config.yaml` — must set `database.default.link`, `jwt.secret`, and `rsa.*` keys before running. Initialize the DB first:

```bash
mysql -u <user> -p < check_list/create_mysql_database.sql
```

### Firmware (`firmware/`)

```bash
python3 ./fetch_repos.py  # fetch git-based component dependencies (run first)
idf.py build              # build
idf.py flash              # flash to connected CoreS3
idf.py monitor            # serial monitor
idf.py flash monitor      # flash then monitor
```

Requires ESP-IDF v5.5.4 with `IDF_PATH` set. Dependencies are fetched via `fetch_repos.py` (not `idf.py recsdeps`) and cloned into `components/` and `xiaozhi-esp32/`.

### Remote Controller (`remote/code/`)

Uses ESP-IDF v5.4.2 targeting esp32 (not esp32-s3). Flash at baud `1500000`. Before building, patch `M5GFX`: globally replace `__has_include(<driver/i2c_master.h>)` with `0`.

## Architecture

### Cross-Component Communication

The four components form this communication graph:

```
Flutter App ←── BLE ──→ Firmware (CoreS3)
Flutter App ←── HTTP/WS ──→ Go Server ←── WebSocket/ESP-NOW ──→ Firmware
Remote Controller ←── ESP-NOW ──→ Firmware
```

The WebSocket endpoint is `ws://<server>/stackChan/ws`. `WsSignalSource` enum in `firmware/main/hal/hal.h` distinguishes messages from the app vs. the remote.

### Server Layer Architecture (GoFrame)

GoFrame enforces a strict layered pattern — do not skip layers:

```
api/          → request/response struct definitions (gf gen api)
controller/   → HTTP handlers, bind to routes in internal/cmd/cmd.go
service/      → interfaces (gf gen service)
logic/        → business logic implementations
dao/          → generated DB accessors (gf gen dao)
model/        → entity and input/output structs
```

Route groups and their middleware:
- `/stackChan/v2/*` — JWT auth via `V2TokenAuthMiddleware`, binds `user`, `dance`, `device` v2 controllers
- `/stackChan/*` — JWT auth via `TokenAuthMiddleware`, binds all other v1 controllers
- `/admin/stackChan/*` — admin JWT auth via `AdminTokenAuthMiddleware`
- `/stackChan/ws` — WebSocket, no route group middleware
- `/file/*` — static file serving from `file/` directory

### Firmware Architecture (Mooncake + HAL)

The firmware uses the [Mooncake](https://github.com/Forairaaaaa/mooncake) app framework. `app_main()` installs all apps into the Mooncake runtime, which calls their `update()` in a loop.

- **HAL** (`firmware/main/hal/`) — hardware abstraction layer; `GetHAL()` is the singleton. All hardware access goes through HAL.
- **StackChan** (`firmware/main/stackchan/`) — the core robot abstraction: attaches `Motion` (servo control) and `Avatar` (face display) instances, applies modifiers.
- **Apps** (`firmware/main/apps/`) — self-contained features: `AppAiAgent`, `AppAvatar`, `AppDance`, `AppEspnowControl`, `AppAppCenter`, `AppEzdata`, `AppSetup`, `AppLauncher`.
- **XiaoZhi** — AI voice assistant, integrated as a patched submodule at `xiaozhi-esp32/` (patch in `patches/`).

### Flutter App Architecture (GetX)

State management uses GetX. `AppState` is the global controller (`Get.find<AppState>()`), initialized in `main()` before `runApp`.

Key modules:
- `lib/network/urls.dart` — **must configure `Urls.url`** with the server IP:port before the app can talk to the server
- `lib/util/value_constant.dart` — RSA key pairs for encrypted communication with the server
- `lib/network/web_socket_util.dart` — WebSocket client connecting to the Go server
- `lib/util/blue_util.dart` — BLE connection to the firmware via `flutter_blue_plus`
- `lib/util/XiaoZhi_util.dart` — XiaoZhi AI cloud service integration (separate from server)

The app communicates with two distinct backends: the self-hosted Go server (`Urls.getBaseUrl()`) and the XiaoZhi AI cloud service (`https://XiaoZhi.me/`).

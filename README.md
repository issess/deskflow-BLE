# Deskflow-BLE

Deskflow (키보드/마우스를 여러 PC 간에 공유하는 오픈소스 도구)의 포크 브랜치로, **기존 TCP/IP 전송 경로 위에 Bluetooth Low Energy(BLE) 전송을 추가 지원**한다. 같은 LAN이 없거나 방화벽/DHCP 제약으로 TCP 연결이 곤란한 환경에서도 두 PC 간 직접 페어링만으로 동작하도록 한다.

원본 프로젝트: [deskflow/deskflow](https://github.com/deskflow/deskflow)

---

## BLE 지원 요약

| 항목 | 내용 |
|---|---|
| 전송 전환 | `Settings::Core::Transport` (`tcp` | `ble`). 기본값 `tcp`이므로 기존 사용자 영향 없음 |
| 페어링 | 호스트가 6자리 PIN을 광고, 원격이 PIN 입력 → 연결 시 검증 |
| 코드 생명주기 | 페어링 세션 시작 시 `QRandomGenerator::system()`으로 재생성, 성공/취소/타임아웃 시 즉시 폐기 |
| 안전성 | PIN은 광고에 평문으로 실리지 않고 SHA-256 앞 4B만 노출. 3회 오답 시 세션 종료 |
| 프레이밍 | ATT MTU 대응 4B big-endian length-prefix, 수신측에서 재조립 후 상위 스트림에 전달 |
| 신뢰기기 기억 | 성공 시 peer UUID를 `Settings::Client::RemoteBleDevice`에 저장, 이후 자동 재연결 |
| 추상화 삽입점 | `IDataSocket` / `IListenSocket` / `ISocketFactory`에 BLE 구현을 주입하여 상위 `Client` / `ServerProxy` / `ClientListener` 로직 무변경 |

구현 모듈:
- `src/lib/ble/` — BLE 전송 전체(`BleSocketFactory`, `BleListenSocket`(peripheral), `BleSocket`(peripheral+central), `BleFraming`, `BlePairingCode`, `BlePairingBroker`, `BleTransport.h`)
- `src/lib/gui/dialogs/BlePairingDialog.{h,cpp}` — 호스트/원격 이중 모드 페어링 GUI
- `src/lib/deskflow/ClientApp.cpp`, `ServerApp.cpp` — transport 설정에 따라 팩토리 분기
- `src/lib/common/Settings.{h,cpp}` — `Core::Transport`, `Client::RemoteBleDevice` 키 추가

---

## 빌드 요구사항

### 공통
- CMake ≥ 3.24
- C++20 컴파일러
- Qt 6.7 이상, `Core Widgets Network Bluetooth` 컴포넌트
- OpenSSL 3.0 이상
- vcpkg (권장: 서브모듈 또는 환경변수 `VCPKG_ROOT`)

### Windows
- Visual Studio 2022 (Desktop development with C++)
- Windows 10/11 + BLE 지원 Bluetooth 어댑터
- vcpkg가 `qtconnectivity`, `qtbase`, `openssl`, `gtest`, `qttranslations`, `qtsvg`를 설치함

### macOS / Linux
- Qt Bluetooth peripheral 역할이 OS별로 차이가 있음 (Linux는 BlueZ, macOS는 CoreBluetooth). 1차 검증 대상은 Windows이며 타 플랫폼은 미검증.

---

## 빌드 (Windows, CMake + vcpkg)

```powershell
# 1. 저장소 준비
git clone https://github.com/<your-org>/deskflow-BLE.git
cd deskflow-BLE

# 2. vcpkg 매니페스트 모드 구성 (vcpkg.json이 자동 생성됨)
#    VCPKG_ROOT 환경변수가 설정되어 있어야 함
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake

# 3. 빌드 (Release)
cmake --build build --config Release --parallel

# 4. (선택) 설치 패키지 생성
cmake --build build --target PACKAGE --config Release
```

첫 구성 시 vcpkg가 Qt와 의존성을 전부 내려받고 빌드하므로 **수십 분 이상** 소요될 수 있다. 성공 시 바이너리는 `build/bin/Release/`에 생성된다.

### Qt 시스템 설치를 쓰고 싶다면
루트 `CMakeLists.txt` 상단의 `option(VCPKG_QT "Use Qt from VCPKG" OFF)`을 OFF로 두고, 시스템 Qt의 `CMAKE_PREFIX_PATH`를 지정:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.7.2/msvc2022_64"
```

이 경우 `qtconnectivity`(QtBluetooth 모듈)이 해당 Qt 설치에 포함되어 있어야 한다.

---

## 실행 및 BLE 페어링 사용법

### 호스트(입력을 공유하는 쪽, 서버)
1. `deskflow.exe` 실행.
2. 메인 창에서 **Server** 모드 선택.
3. `Edit → BLE Pairing…` 클릭 — 다이얼로그가 열리고 전송 설정이 자동으로 `ble`로 전환된다.
4. `Start`(또는 `Ctrl+S`)로 코어 시작 — BLE 광고가 시작되며 6자리 코드가 다이얼로그에 큰 글씨로 표시된다.
5. 원격이 코드를 입력해 연결하면 다이얼로그가 자동으로 닫히고 일반 데스크플로우 세션이 시작된다.
6. 페어링이 120초 내에 완료되지 않으면 코드가 폐기되며, 실패 시 코드를 재생성해 다시 시도.

### 원격(입력을 받는 쪽, 클라이언트)
1. `deskflow.exe` 실행.
2. 메인 창에서 **Client** 모드 선택.
3. `Edit → BLE Pairing…` — 6자리 입력 다이얼로그가 열린다 (Transport는 자동으로 `ble`).
4. 호스트 화면에 표시된 6자리 코드를 입력하고 **Pair**.
5. `Start`(또는 `Ctrl+S`)로 코어 시작 — 스캔 → 해시 매칭 → 연결 → 코드 검증이 자동 진행된다.
6. 성공 시 peer UUID가 저장되어 **다음 실행부터는 코드 입력 없이** 같은 호스트에 자동 재연결된다.
7. 호스트를 바꾸거나 기억된 기기를 지우려면 `Settings::Client::RemoteBleDevice` 값을 비우거나 페어링 다이얼로그에서 새 코드로 다시 수행.

### TCP로 되돌리려면
Preferences(설정)에서 Transport를 `tcp`로 변경하거나, `Settings::Core::Transport` 키를 `tcp`로 덮어쓴 뒤 코어 재시작.

---

## 보안 고려사항

- 6자리 PIN 공간은 10⁶. 3회 오답 시 세션이 종료되어 온라인 brute-force는 실질적으로 봉쇄.
- 광고 payload에는 PIN 해시의 앞 4B만 실리므로 평문 노출 없음. 우연 충돌 확률 1/2³².
- BLE 링크레이어 Just-Works는 능동 MITM에 약하므로 **앱계층 PIN 검증이 주된 방어선**이다. 공용 공간에서 페어링할 때는 육안으로 코드를 전달할 것.
- 저장된 신뢰 기기(peer UUID) 목록은 사용자가 언제든 지울 수 있어야 한다 (현재는 Settings 직접 편집으로 가능, GUI 관리 UI는 향후 추가 예정).
- 이 포크는 BLE 위에 TLS/SecureSocket을 중첩하지 않는다. 고위협 환경에서는 기존 TCP+TLS 경로 사용 권장.

---

## 프로젝트 구조 (BLE 관련)

```
src/lib/ble/
├── BleTransport.h            # Service/Characteristic UUID, 광고 매직, 상수
├── BlePairingCode.{h,cpp}    # 6자리 PIN 생성/검증/폐기 + SHA-256 접두 해시
├── BleFraming.{h,cpp}        # 4B length-prefixed 프레이밍 writer/reader
├── BlePairingBroker.{h,cpp}  # PIN 및 결과를 GUI ↔ 전송계층 전달하는 QObject 싱글톤
├── BleSocket.{h,cpp}         # IDataSocket 구현 (central+peripheral 양방향)
├── BleListenSocket.{h,cpp}   # IListenSocket 구현 (peripheral + advertising)
├── BleSocketFactory.{h,cpp}  # ISocketFactory 구현
└── CMakeLists.txt
```

GATT 스키마:

| Characteristic | 방향 | 용도 |
|---|---|---|
| `PairingAuth` | central → peripheral (write) | 6자리 PIN 전송 |
| `PairingStatus` | peripheral → central (notify) | Accepted / Rejected |
| `DataDownstream` | peripheral → central (notify) | 스트림 데이터 (host → remote) |
| `DataUpstream` | central → peripheral (write) | 스트림 데이터 (remote → host) |
| `Control` | peripheral → central (notify) | 예약 (disconnect 사유 등) |

---

## 개발 & 기여

- 단위 테스트: `src/unittests/` (gtest 기반) — BLE 모듈용 테스트는 추가 예정 (BlePairingCode / BleFraming).
- 로깅: 기존 `CLOG_*` 매크로 사용. 전송 디버깅 시 로그 레벨을 DEBUG2 이상으로.
- 원본 프로젝트의 코딩 컨벤션과 라이선스(GPL-2.0 with OpenSSL exception, 신규 파일도 동일)를 따름.

### 현재 상태

완료:
- 전송 추상화 주입, Transport 설정 분기
- BLE peripheral(호스트) 광고 + 페어링 검증
- BLE central(원격) 스캔 + 해시 매칭 + 자동 재연결
- 페어링 다이얼로그 GUI

미완 / 향후:
- `BlePairingCode`, `BleFraming` 단위 테스트
- 2대 Windows 기기에서의 end-to-end 실기 검증
- 신뢰 기기 목록 관리 GUI
- macOS/Linux 포팅 검증

---

## 라이선스

원본 Deskflow와 동일한 [GPL-2.0-only WITH LicenseRef-OpenSSL-Exception](LICENSE). 신규 추가된 BLE 관련 소스도 동일 라이선스.

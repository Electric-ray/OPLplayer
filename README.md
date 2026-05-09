# OPLplayer

DOS 시대 OPL3(YMF262) 사운드를 실시간으로 재생하는 플레이어입니다.  
ESP32 adlcom ElectricRay 보드 → WiFi UDP / OTG USB 시리얼 → YMFM 에뮬레이션 → 오디오 출력

---

## 구성

| 폴더 | 내용 |
|------|------|
| `OPLplayer-Windows/` | Windows x86 데스크탑 앱 (Visual Studio 2022) |
| `OPLplayer-Android/` | Android 앱 (Android Studio / NDK) |

---

## OPLplayer-Windows

### 빌드 환경
- Visual Studio 2022, Windows SDK 10.0, x86 Release
- PortAudio (정적 라이브러리, `3rdparty/portaudio/` 포함)
- YMFM 에뮬레이터 소스 포함

### 빌드 방법
```
OPLplayer.sln 열기 → Release | Win32 → 빌드
```
`portaudio.dll` 을 실행 파일과 같은 폴더에 배치하세요.

### 기능
- ESP32 UDP 수신 (포트 9800)
- COM 포트 직접 수신 (adlcom ElectricRay Nuke 3바이트 프로토콜 @ 115200bps)
- 사인톤 테스트 / 볼륨·게인 슬라이더 / 타이밍 프리셋
- OPL3 칩 초기화

---

## OPLplayer-Android

### 빌드 환경
- Android Studio Hedgehog 이상
- NDK r25+, CMake 3.22+
- minSdk 24 (Android 7.0)

### 빌드 방법
```
Android Studio → Open → OPLplayer-Android 폴더
Sync → Run ▶
```

### 기능
- ESP32 WiFi UDP 수신 (WiFi 네트워크 강제 바인딩)
- OTG USB 시리얼 수신 (FTDI / CP210x / CH34x / PL2303)
- 볼륨·게인 슬라이더 / 버퍼링 프리셋
- 수신 LED 인디케이터

---

## 하드웨어

| 항목 | 내용 |
|------|------|
| OPL3 보드 | adlcom ElectricRay (YMF262 + ESP32) |
| 연결 방식 | WiFi (ESP32 AP 모드, 192.168.4.1:9800) 또는 USB 시리얼 |
| 프로토콜 | Nuke.YKT 3바이트 OPL3 write 패킷 |

---

## 라이선스

- YMFM 에뮬레이터: [Apache 2.0](https://github.com/aaronsgiles/ymfm)
- PortAudio: MIT License
- usb-serial-for-android: LGPL 2.1
- Oboe: Apache 2.0
- 이 프로젝트 코드: MIT License

---

*2026 전기가오리 / 네이버카페 도스박물관*

// ============================================================================
// OPLplayer - adlcom ElectricRay + ESP32 OPL Timing Bridge 전용
//  - YMFM YMF262 (OPL3) 에뮬레이션
//  - ESP32 UDP 포트 9800 실시간 수신 재생
//  - COM 포트 직접 수신 (adlcom ElectricRay Nuke 3바이트 프로토콜)
//  - 사인톤 테스트 / 볼륨 + 게인 슬라이더 / 타이밍 프리셋
//  버스트 개선: COM cursor 클램핑, UDP deltaUs 패킷내 상대 분산
// ============================================================================

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#pragma execution_character_set("utf-8")

#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <uxtheme.h>
#include <dwmapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "portaudio_x86.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>

#include "portaudio.h"
#include "ymfm_opl.h"

// ------------------------------------------------------------
// 컬러
// ------------------------------------------------------------
#define APPLE_BLUE         RGB(0, 122, 255)
#define APPLE_BLUE_HOVER   RGB(51, 153, 255)
#define APPLE_BLUE_PRESS   RGB(0, 102, 204)
#define APPLE_BG           RGB(245, 245, 247)
#define APPLE_WHITE        RGB(255, 255, 255)
#define APPLE_TEXT         RGB(28, 28, 30)
#define APPLE_GREEN        RGB(52, 199, 89)
#define APPLE_GRAY         RGB(80, 80, 80)

static HBRUSH g_hBgBrush       = nullptr;
static HBRUSH g_hSliderBgBrush = nullptr;

// ------------------------------------------------------------
// 오디오 설정
// ------------------------------------------------------------
static double              kSampleRate      = 49716.0;
static const unsigned long kFramesPerBuffer = 256;

// ------------------------------------------------------------
// YMFM 인터페이스 + OPL3 칩
// ------------------------------------------------------------
class ymfm_interface_impl final : public ymfm::ymfm_interface {
public:
    void    ymfm_set_timer(uint32_t, int32_t) override {}
    void    ymfm_set_busy_end(uint32_t) override {}
    bool    ymfm_is_busy() override { return false; }
    void    ymfm_update_irq(bool) override {}
    uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
    void    ymfm_external_write(ymfm::access_class, uint32_t, uint8_t) override {}
};

static ymfm_interface_impl g_intf;
static ymfm::ymf262        g_chip(g_intf);

static inline void WriteOPL3Reg(uint16_t reg, uint8_t value)
{
    uint8_t  regAddr  = (uint8_t)(reg & 0xFF);
    // OPL2 호환: 0xC0-0xC8 패닝 비트 없으면 양쪽 출력 강제 (IMPlay 등 OPL2 포맷 대응)
    if (regAddr >= 0xC0 && regAddr <= 0xC8 && (value & 0x30) == 0) value |= 0x30;
    uint8_t  bank     = (uint8_t)((reg >> 8) & 1);
    uint32_t addrPort = (uint32_t)(bank * 2);
    g_chip.write(addrPort,     regAddr);
    g_chip.write(addrPort + 1, value);
}

static void InitOPL3()
{
    g_chip.reset();
    g_chip.write(2, 0x05);
    g_chip.write(3, 0x01);
    WriteOPL3Reg(0x001, 0x20);
    for (int i = 0; i < 9; ++i) {
        WriteOPL3Reg((uint16_t)(0x0B0 + i), 0x00);
        WriteOPL3Reg((uint16_t)(0x1B0 + i), 0x00);
    }
}

// ------------------------------------------------------------
// 라이브 이벤트 링버퍼
// ------------------------------------------------------------
struct LiveEvent {
    uint64_t sample;
    uint16_t reg;
    uint8_t  val;
};

static const uint32_t        kLiveRingSize = 1u << 17;
static LiveEvent             g_liveRing[kLiveRingSize];
static std::atomic<uint32_t> g_liveWriteIndex(0);
static std::atomic<uint32_t> g_liveReadIndex(0);

static std::atomic<bool>     g_playFromNetwork(false);
static std::atomic<uint64_t> g_playheadSample(0);
static std::atomic<uint64_t> g_eventCursor(0);

static std::atomic<uint64_t> g_udpStreamClock(0); // UDP 전용 누적 스트림 클럭
static std::atomic<uint64_t> g_udpBytes(0);
static std::atomic<uint64_t> g_udpPackets(0);
static std::atomic<uint64_t> g_udpEvents(0);
static std::atomic<uint64_t> g_udpDrops(0);
static std::atomic<uint64_t> g_udpSeqGaps(0);
static std::atomic<uint64_t> g_udpDupPackets(0);
static std::atomic<uint64_t> g_udpOutOfOrder(0);

// 링버퍼 push 전용 - 커서 조작 없음
static void PushLiveEvent(uint64_t sample, uint16_t reg, uint8_t val)
{
    uint32_t w     = g_liveWriteIndex.load(std::memory_order_relaxed);
    uint32_t r     = g_liveReadIndex.load(std::memory_order_acquire);
    uint32_t nextW = (w + 1) & (kLiveRingSize - 1);
    if (nextW == r) {
        g_udpDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_liveRing[w] = { sample, reg, val };
    g_liveWriteIndex.store(nextW, std::memory_order_release);
}

static void ClearLiveRing()
{
    uint32_t w = g_liveWriteIndex.load(std::memory_order_relaxed);
    g_liveReadIndex.store(w, std::memory_order_release);
}

static std::atomic<bool> g_appQuit(false);
// 링버퍼 flush 후 커서 리셋 (순서 역전 방지를 위해 항상 flush와 함께)
static void SafeResetCursor(uint64_t prebufSamples)
{
    ClearLiveRing();
    uint64_t ph = g_playheadSample.load(std::memory_order_relaxed);
    g_eventCursor.store(ph + prebufSamples, std::memory_order_relaxed);
}


// ------------------------------------------------------------
// Nuke.YKT 3바이트 디코더 (COM 포트용)
// ------------------------------------------------------------
class NukeDecoder {
public:
    enum class State { WaitStart, GotByte0, GotByte1 };

    void reset() {
        m_state = State::WaitStart;
        m_highBank = false; m_reg = 0; m_val = 0;
        m_syncErrors = 0; m_packetsDecoded = 0;
    }

    bool processByte(uint8_t byte, uint16_t& outReg, uint8_t& outVal) {
        if (byte & 0x80) {
            if (m_state != State::WaitStart) m_syncErrors++;
            uint8_t addrShift6 = byte & 0x0F;
            m_highBank = (addrShift6 >= 4);
            m_reg      = (addrShift6 & 0x03) << 6;
            m_state    = State::GotByte0;
            return false;
        }
        switch (m_state) {
        case State::WaitStart: break;
        case State::GotByte0:
            m_reg |= (byte >> 1) & 0x3F;
            m_val  = (byte & 0x01) << 7;
            m_state = State::GotByte1;
            break;
        case State::GotByte1:
            m_val |= byte & 0x7F;
            outReg = m_reg | (m_highBank ? 0x0100u : 0u);
            outVal = m_val;
            m_packetsDecoded++;
            m_state = State::WaitStart;
            return true;
        }
        return false;
    }

    uint64_t syncErrors()     const { return m_syncErrors; }
    uint64_t packetsDecoded() const { return m_packetsDecoded; }

private:
    State    m_state    = State::WaitStart;
    bool     m_highBank = false;
    uint8_t  m_reg = 0, m_val = 0;
    uint64_t m_syncErrors = 0, m_packetsDecoded = 0;
};

// ------------------------------------------------------------
// COM 포트 전역
// ------------------------------------------------------------
static std::atomic<bool>      g_comReceiveEnabled(false);
static std::atomic<ULONGLONG> g_lastComActivityMs(0);
static std::atomic<uint64_t>  g_comBytes(0);
static std::atomic<uint64_t>  g_comEvents(0);
static std::atomic<uint64_t>  g_comErrors(0);
static HANDLE                 g_hCom    = INVALID_HANDLE_VALUE;
static std::thread            g_comThread;
static const int              kComPrebufMs = 30;

static std::vector<std::wstring> EnumComPorts()
{
    std::vector<std::wstring> result;
    for (int i = 1; i <= 32; ++i) {
        wchar_t name[32];
        swprintf_s(name, L"\\\\.\\COM%d", i);
        HANDLE h = CreateFileW(name, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            wchar_t friendly[16];
            swprintf_s(friendly, L"COM%d", i);
            result.push_back(friendly);
        }
    }
    return result;
}

static bool OpenComPort(const wchar_t* portName)
{
    if (g_hCom != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hCom); g_hCom = INVALID_HANDLE_VALUE;
    }
    std::wstring path = std::wstring(L"\\\\.\\") + portName;
    g_hCom = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_hCom == INVALID_HANDLE_VALUE) return false;

    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    GetCommState(g_hCom, &dcb);
    dcb.BaudRate = 115200; dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE; dcb.fParity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(g_hCom, &dcb)) {
        CloseHandle(g_hCom); g_hCom = INVALID_HANDLE_VALUE; return false;
    }
    COMMTIMEOUTS ct{};
    ct.ReadIntervalTimeout      = 10;
    ct.ReadTotalTimeoutConstant = 50;
    SetCommTimeouts(g_hCom, &ct);
    PurgeComm(g_hCom, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

// ------------------------------------------------------------
// COM 수신 스레드 - 버스트 개선: ReadFile 직후 cursor 클램핑
// 115200bps 기준 1이벤트(3byte) = ~260us = ~13샘플@49716Hz
// kMinStep=26 (2배 여유), cursor가 prebuf+200ms 초과시 리셋
// ------------------------------------------------------------
// COM 수신 스레드
//  kMinStep=13: 115200bps 3byte/event 물리 한계 (12.95 samples)
//  커서가 prebuf+300ms 초과 시 링flush+리셋 (순서 역전 방지)
static void ComReceiverThread()
{
    NukeDecoder dec; dec.reset();
    uint8_t buf[256];

    const uint64_t kPrebuf   = (uint64_t)(kSampleRate * kComPrebufMs / 1000.0);
    const uint64_t kMaxAhead = kPrebuf + (uint64_t)(kSampleRate * 0.3); // prebuf+300ms
    const uint64_t kMinStep  = 13; // 115200bps, 3byte, 10bit/byte = 12.95 samples

    while (!g_appQuit.load() && g_comReceiveEnabled.load())
    {
        if (g_hCom == INVALID_HANDLE_VALUE) { Sleep(10); continue; }
        DWORD got = 0;
        if (!ReadFile(g_hCom, buf, sizeof(buf), &got, nullptr)) break;
        if (got == 0) continue;

        g_comBytes.fetch_add(got, std::memory_order_relaxed);
        g_lastComActivityMs.store(GetTickCount64(), std::memory_order_relaxed);

        uint64_t ph  = g_playheadSample.load(std::memory_order_relaxed);
        uint64_t cur = g_eventCursor.load(std::memory_order_relaxed);

        // 커서가 너무 앞서면 링 flush + 리셋 (역전 방지)
        if (cur > ph + kMaxAhead) {
            SafeResetCursor(kPrebuf);
            ph  = g_playheadSample.load(std::memory_order_relaxed);
            cur = g_eventCursor.load(std::memory_order_relaxed);
        }

        for (DWORD i = 0; i < got; ++i) {
            uint16_t reg; uint8_t val;
            if (dec.processByte(buf[i], reg, val)) {
                uint64_t next = cur + kMinStep;
                uint64_t floor_ = ph + kPrebuf;
                if (next < floor_) next = floor_;
                cur = next;
                g_eventCursor.store(cur, std::memory_order_relaxed);
                PushLiveEvent(cur, reg, val);
                g_comEvents.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (dec.syncErrors() > g_comErrors.load())
                    g_comErrors.store(dec.syncErrors());
            }
        }
    }
    g_comReceiveEnabled.store(false);
}

// ------------------------------------------------------------
// UDP 전역
// ------------------------------------------------------------
static std::atomic<bool>      g_udpReceiveEnabled(false);
static std::atomic<ULONGLONG> g_lastUdpActivityMs(0);

static std::atomic<int>       g_timingMode(1);
static std::atomic<int>       g_prebufferMs(50);
static std::atomic<int>       g_minEventUs(220);
static std::atomic<int>       g_gapPreserveUs(10000);
static std::atomic<uint32_t>  g_timingEpoch(0);

static std::atomic<bool>      g_hasBaseTime(false);
static std::atomic<uint64_t>  g_baseRemoteUs(0);
static std::atomic<uint64_t>  g_baseSample(0);

static std::atomic<double>    g_userVolume(1.0);
static std::atomic<double>    g_userGain(1.0);
static const double           kMasterGain = 0.5;

static std::thread            g_udpThread;
static PaStream*              g_paStream = nullptr;

// ------------------------------------------------------------
// Win32 UI 상태
// ------------------------------------------------------------
struct UIState {
    HWND hMain        = nullptr;
    HWND hBtnSine     = nullptr;
    HWND hBtnStop     = nullptr;
    HWND hBtnUdp      = nullptr;
    HWND hBtnCom      = nullptr;
    HWND hComboPort   = nullptr;
    HWND hBtnReset    = nullptr;
    HWND hLog         = nullptr;
    HWND hLedUdp      = nullptr;
    HWND hPresetCombo = nullptr;
    HWND hUdpStats    = nullptr;
    HWND hSliderVol   = nullptr;
    HWND hSliderGain  = nullptr;
} G;

enum CtrlId : int {
    IDC_BTN_SINE    = 1001,
    IDC_BTN_STOP    = 1004,
    IDC_BTN_UDP     = 1005,
    IDC_BTN_RESET   = 1006,
    IDC_BTN_COM     = 1007,
    IDC_COMBO_PORT  = 1009,
    IDC_LOG         = 2000,
    IDC_LED_UDP     = 2001,
    IDC_SLIDER_VOL  = 3000,
    IDC_SLIDER_GAIN = 3001,
    IDC_PRESET      = 3002,
    IDC_UDP_STATS   = 3003
};

static void LogLine(const wchar_t* fmt, ...)
{
    if (!G.hLog) return;
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    int idx = (int)SendMessageW(G.hLog, LB_ADDSTRING, 0, (LPARAM)buf);
    SendMessageW(G.hLog, LB_SETTOPINDEX, idx, 0);
}

static void TestSine()
{
    g_playFromNetwork.store(false, std::memory_order_relaxed);
    InitOPL3();
    LogLine(L"[테스트] OPL3 사인톤 시작 (A4=440Hz, 1초)");
    WriteOPL3Reg(0x20, 0x01); WriteOPL3Reg(0x40, 0x3F);
    WriteOPL3Reg(0x60, 0xF0); WriteOPL3Reg(0x80, 0x0F);
    WriteOPL3Reg(0xE0, 0x00);
    WriteOPL3Reg(0x23, 0x01); WriteOPL3Reg(0x43, 0x00);
    WriteOPL3Reg(0x63, 0xF0); WriteOPL3Reg(0x83, 0x07);
    WriteOPL3Reg(0xE3, 0x00);
    WriteOPL3Reg(0xC0, 0x31);
    const uint16_t fnum  = 0x244;
    const uint8_t  block = 4;
    WriteOPL3Reg(0xA0, fnum & 0xFF);
    WriteOPL3Reg(0xB0, (uint8_t)(0x20 | ((block & 7) << 2) | ((fnum >> 8) & 0x03)));
    Sleep(1000);
    WriteOPL3Reg(0xB0, (uint8_t)(((block & 7) << 2) | ((fnum >> 8) & 0x03)));
    LogLine(L"[테스트] 사인톤 종료");
}

static void PrepareUdpOn()
{
    g_udpStreamClock.store(0, std::memory_order_relaxed);
    g_liveWriteIndex.store(0, std::memory_order_relaxed);
    g_liveReadIndex.store(0,  std::memory_order_relaxed);
    g_udpBytes.store(0);    g_udpPackets.store(0);
    g_udpEvents.store(0);   g_udpDrops.store(0);
    g_udpSeqGaps.store(0);  g_udpDupPackets.store(0);
    g_udpOutOfOrder.store(0);
    g_hasBaseTime.store(false);
    g_baseRemoteUs.store(0); g_baseSample.store(0);
}

static void RefreshComPorts()
{
    if (!G.hComboPort) return;
    SendMessageW(G.hComboPort, CB_RESETCONTENT, 0, 0);
    SendMessageW(G.hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- 포트 --");
    auto ports = EnumComPorts();
    for (auto& p : ports)
        SendMessageW(G.hComboPort, CB_ADDSTRING, 0, (LPARAM)p.c_str());
    SendMessageW(G.hComboPort, CB_SETCURSEL, 0, 0);
    LogLine(L"[COM] 포트 목록 갱신: %zu개 발견", ports.size());
}

static void StopComReceive()
{
    g_comReceiveEnabled.store(false);
    if (g_comThread.joinable()) g_comThread.join();
    if (g_hCom != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hCom); g_hCom = INVALID_HANDLE_VALUE;
    }
    if (G.hBtnCom) SetWindowTextW(G.hBtnCom, L"COM OPL3 ON");
}

static void StopAllPlayback()
{
    g_playFromNetwork.store(false, std::memory_order_relaxed);
    g_udpReceiveEnabled.store(false, std::memory_order_relaxed);
    StopComReceive();
    PrepareUdpOn();
    g_playheadSample.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 9; ++i) {
        WriteOPL3Reg((uint16_t)(0x0B0 + i), 0x00);
        WriteOPL3Reg((uint16_t)(0x1B0 + i), 0x00);
    }
    if (G.hBtnUdp) SetWindowTextW(G.hBtnUdp, L"UDP OPL3 ON");
    if (G.hLedUdp) InvalidateRect(G.hLedUdp, nullptr, TRUE);
    LogLine(L"[재생] 정지");
}

static void ApplyLivePreset(int sel)
{
    switch (sel) {
    default:
    case 0:
        g_timingMode.store(1); g_prebufferMs.store(50);
        g_minEventUs.store(220); g_gapPreserveUs.store(10000);
        LogLine(L"[프리셋] 밸런스(권장): prebuf=50ms, minΔ=220us");
        break;
    case 1:
        g_timingMode.store(1); g_prebufferMs.store(80);
        g_minEventUs.store(260); g_gapPreserveUs.store(12000);
        LogLine(L"[프리셋] 지터 최소: prebuf=80ms, minΔ=260us");
        break;
    case 2:
        g_timingMode.store(1); g_prebufferMs.store(120);
        g_minEventUs.store(180); g_gapPreserveUs.store(15000);
        LogLine(L"[프리셋] 누락 방지: prebuf=120ms");
        break;
    case 3:
        g_timingMode.store(2); g_prebufferMs.store(70);
        g_minEventUs.store(220); g_gapPreserveUs.store(10000);
        LogLine(L"[프리셋] 갭 보존");
        break;
    case 4:
        g_timingMode.store(0); g_prebufferMs.store(40);
        g_minEventUs.store(0);  g_gapPreserveUs.store(0);
        LogLine(L"[프리셋] Raw (그대로)");
        break;
    }
    g_hasBaseTime.store(false); g_baseRemoteUs.store(0); g_baseSample.store(0);
    g_timingEpoch.fetch_add(1, std::memory_order_relaxed);
}

// ------------------------------------------------------------
// PortAudio 콜백
// ------------------------------------------------------------
static int PaCallback(const void*, void* output,
    unsigned long frameCount,
    const PaStreamCallbackTimeInfo*,
    PaStreamCallbackFlags, void*)
{
    float* out = (float*)output;
    for (unsigned long i = 0; i < frameCount; ++i)
    {
        uint64_t curSample = g_playheadSample.fetch_add(1, std::memory_order_relaxed);
        if (g_playFromNetwork.load(std::memory_order_relaxed))
        {
            uint32_t r = g_liveReadIndex.load(std::memory_order_relaxed);
            uint32_t w = g_liveWriteIndex.load(std::memory_order_acquire);
            while (r != w) {
                if (g_liveRing[r].sample > curSample) break;
                WriteOPL3Reg(g_liveRing[r].reg, g_liveRing[r].val);
                r = (r + 1) & (kLiveRingSize - 1);
            }
            g_liveReadIndex.store(r, std::memory_order_release);
        }
        ymfm::ymf262::output_data od{};
        g_chip.generate(&od);
        double L = (double)od.data[0] + (double)od.data[2];
        double R = (double)od.data[1] + (double)od.data[3];
        double vol  = g_userVolume.load(std::memory_order_relaxed);
        double gain = g_userGain.load(std::memory_order_relaxed);
        const double scale = kMasterGain * vol * gain / 32768.0;
        L *= scale; R *= scale;
        auto clip = [](double x){ return x>1.0?1.0:(x<-1.0?-1.0:x); };
        *out++ = (float)clip(L);
        *out++ = (float)clip(R);
    }
    return paContinue;
}

// ------------------------------------------------------------
// UDP 수신 스레드
//  버스트 개선: 패킷 내 deltaUs를 패킷 기준 상대 타이밍으로 활용
//   → DOS 원본 타이밍 비율 보존 + 절대 drift 방지
//   → cursor 클램핑: playhead+ahead+300ms 초과 시 리셋
// ------------------------------------------------------------
static void UdpReceiverThread()
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return; }

    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(9800);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s); WSACleanup(); return;
    }

    sockaddr_in esp{};
    esp.sin_family = AF_INET;
    InetPtonA(AF_INET, "192.168.4.1", &esp.sin_addr);
    esp.sin_port = htons(9800);

    ULONGLONG lastHelloMs = 0;
    bool gotAnyPacket = false;
    std::vector<uint8_t> buf(4096);

    bool     haveLastSeq = false;
    uint16_t lastSeq     = 0;
    uint32_t lastEpoch   = 0;

    while (!g_appQuit.load(std::memory_order_relaxed))
    {
        uint32_t epoch = g_timingEpoch.load(std::memory_order_relaxed);
        if (epoch != lastEpoch) {
            lastEpoch = epoch; haveLastSeq = false;
        }

        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        timeval tv{ 0, 50*1000 };
        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            ULONGLONG nowMs = GetTickCount64();
            if (!gotAnyPacket && (nowMs - lastHelloMs) > 300) {
                const char hello[] = "HELLO";
                sendto(s, hello, (int)(sizeof(hello)-1), 0, (sockaddr*)&esp, sizeof(esp));
                lastHelloMs = nowMs;
            }
            continue;
        }

        sockaddr_in from{}; int fromlen = sizeof(from);
        int ret = recvfrom(s, (char*)buf.data(), (int)buf.size(), 0,
                           (sockaddr*)&from, &fromlen);
        if (ret < 8) continue;
        if (buf[0]!='O'||buf[1]!='P'||buf[2]!='L'||buf[3]!='3') continue;

        g_udpBytes.fetch_add((uint64_t)ret, std::memory_order_relaxed);
        g_udpPackets.fetch_add(1, std::memory_order_relaxed);
        gotAnyPacket = true;
        g_lastUdpActivityMs.store(GetTickCount64(), std::memory_order_relaxed);

        uint8_t  type  = buf[4];
        uint8_t  count = buf[5];
        uint16_t seq   = (uint16_t)(buf[6] | (buf[7]<<8));

        if (type == 1) {
            haveLastSeq = false;
            g_hasBaseTime.store(false); g_baseRemoteUs.store(0); g_baseSample.store(0);
            continue;
        }
        if (type != 0 || count == 0) continue;
        if (!g_udpReceiveEnabled.load(std::memory_order_relaxed)) continue;

        if (haveLastSeq) {
            if (seq == lastSeq) { g_udpDupPackets.fetch_add(1); continue; }
            uint16_t diff = (uint16_t)(seq - lastSeq);
            if (diff & 0x8000) { g_udpOutOfOrder.fetch_add(1); continue; }
            if (diff != 1) {
                g_udpSeqGaps.fetch_add(1);
                ClearLiveRing();
                haveLastSeq = false;
            }
        }
        haveLastSeq = true; lastSeq = seq;

        const int      pbMs    = std::max(20, std::min(200, (int)g_prebufferMs.load()));
        const uint64_t kPrebuf = (uint64_t)(kSampleRate * pbMs / 1000.0);
        const uint64_t kMax    = kPrebuf + (uint64_t)(kSampleRate * 0.3);

        uint64_t ph  = g_playheadSample.load(std::memory_order_relaxed);
        uint64_t clk = g_udpStreamClock.load(std::memory_order_relaxed);

        // 너무 앞서면 ring flush + 리셋
        if (clk > ph + kMax) {
            SafeResetCursor(kPrebuf);
            ph  = g_playheadSample.load(std::memory_order_relaxed);
            clk = ph + kPrebuf;
        }
        // 너무 뒤처지면 prebuf 위치로 catch-up
        if (clk < ph + kPrebuf) clk = ph + kPrebuf;

        // deltaUs 패킷 경계 넘어 누적 -> 음 길이 보존
        uint64_t prevClk = clk;
        size_t   off = 8;
        for (uint32_t i = 0; i < (uint32_t)count; ++i)
        {
            if (off + 6 > (size_t)ret) break;
            uint16_t deltaUs = (uint16_t)(buf[off+0] | (buf[off+1]<<8));
            uint16_t reg     = (uint16_t)(buf[off+2] | (buf[off+3]<<8));
            uint8_t  val     = buf[off+4];
            off += 6;
            uint8_t  bank01  = (uint8_t)((reg >> 8) & 0x01);
            uint8_t  reg8    = (uint8_t)(reg & 0xFF);
            uint16_t regFull = (uint16_t)(reg8 | (bank01 ? 0x100u : 0x000u));
            clk += (uint64_t)((double)deltaUs * kSampleRate / 1000000.0);
            if (clk <= prevClk) clk = prevClk + 1;
            prevClk = clk;
            PushLiveEvent(clk, regFull, val);
            g_udpEvents.fetch_add(1, std::memory_order_relaxed);
        }
        g_udpStreamClock.store(clk, std::memory_order_relaxed);

    }

    closesocket(s);
    WSACleanup();
}

// ------------------------------------------------------------
// LED + Owner-draw
// ------------------------------------------------------------
static void DrawLED(HDC hdc, RECT rc, bool active)
{
    COLORREF c  = active ? APPLE_GREEN : APPLE_GRAY;
    HBRUSH hBr  = CreateSolidBrush(c);
    HPEN   hPn  = CreatePen(PS_SOLID, 1, c);
    SelectObject(hdc, hBr); SelectObject(hdc, hPn);
    Ellipse(hdc, rc.left, rc.top, rc.right, rc.bottom);
    DeleteObject(hPn); DeleteObject(hBr);
}

static void HandleDrawItem(LPDRAWITEMSTRUCT dis)
{
    if (dis->CtlType == ODT_BUTTON) {
        HDC  hdc = dis->hDC;
        RECT rc  = dis->rcItem;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool focused = (dis->itemState & ODS_FOCUS)    != 0;
        COLORREF bg = pressed ? APPLE_BLUE_PRESS :
                      focused ? APPLE_BLUE_HOVER : APPLE_BLUE;
        HBRUSH hBr = CreateSolidBrush(bg);
        HPEN   hPn = CreatePen(PS_SOLID, 0, bg);
        SelectObject(hdc, hBr); SelectObject(hdc, hPn);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
        wchar_t text[256];
        GetWindowTextW(dis->hwndItem, text, 256);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255,255,255));
        HFONT hFont = CreateFontW(15,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"맑은 고딕");
        HFONT hOld = (HFONT)SelectObject(hdc, hFont);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(hdc, hOld);
        DeleteObject(hFont); DeleteObject(hPn); DeleteObject(hBr);
    }
    else if (dis->CtlType == ODT_STATIC && dis->CtlID == IDC_LED_UDP) {
        bool active =
            (g_udpReceiveEnabled.load() && (GetTickCount64() - g_lastUdpActivityMs.load() < 300)) ||
            (g_comReceiveEnabled.load() && (GetTickCount64() - g_lastComActivityMs.load() < 300));
        DrawLED(dis->hDC, dis->rcItem, active);
    }
}

// ------------------------------------------------------------
// BuildUI
//  버튼 행 정렬:
//   위쪽: 사인톤(x=20,w=160) / 정지(x=200,w=140) / 칩초기화(x=360,w=160)
//   아래: UDP_ON(x=20,w=130) / LED(x=158,w=22) / COM포트(x=188,w=194) / COM_ON(x=390,w=130)
//   → 양쪽 끝 x=20 ~ x=520 으로 위/아래 행 정렬 일치
// ------------------------------------------------------------
static void BuildUI(HWND hwnd)
{
    G.hMain = hwnd;
    int y = 20;

    // 타이틀
    HWND hTitle = CreateWindowW(L"STATIC", L"OPLplayer",
        WS_CHILD|WS_VISIBLE|SS_CENTER, 0, y, 560, 40, hwnd, nullptr,nullptr,nullptr);
    HFONT hTF = CreateFontW(30,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_DONTCARE,L"맑은 고딕");
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)hTF, TRUE);
    y += 50;

    // ── 행 1: 사인톤 / 정지 / 칩초기화  (x:20~520) ──────────
    G.hBtnSine = CreateWindowW(L"BUTTON", L"사인톤 테스트",
        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        20, y, 160, 40, hwnd, (HMENU)(INT_PTR)IDC_BTN_SINE, nullptr,nullptr);
    G.hBtnStop = CreateWindowW(L"BUTTON", L"정지",
        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        200, y, 140, 40, hwnd, (HMENU)(INT_PTR)IDC_BTN_STOP, nullptr,nullptr);
    G.hBtnReset = CreateWindowW(L"BUTTON", L"칩 초기화",
        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        360, y, 160, 40, hwnd, (HMENU)(INT_PTR)IDC_BTN_RESET, nullptr,nullptr);
    y += 60;

    // 레이블
    CreateWindowW(L"STATIC", L"UDP/COM 수신  (포트 9800 / adlcom 직접)",
        WS_CHILD|WS_VISIBLE, 20, y, 520, 18, hwnd,nullptr,nullptr,nullptr);
    y += 22;

    // ── 행 2: UDP_ON / LED / COM포트콤보 / COM_ON  (x:20~520) ─
    G.hBtnUdp = CreateWindowW(L"BUTTON", L"UDP OPL3 ON",
        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        20, y, 130, 38, hwnd, (HMENU)(INT_PTR)IDC_BTN_UDP, nullptr,nullptr);

    G.hLedUdp = CreateWindowW(L"STATIC", L"",
        WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,
        158, y+8, 22, 22, hwnd, (HMENU)(INT_PTR)IDC_LED_UDP, nullptr,nullptr);

    // COM 포트 콤보: x=188, w=194 → 끝=382, 다음 갭 8px → COM버튼 x=390
    G.hComboPort = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        188, y+4, 194, 200, hwnd, (HMENU)(INT_PTR)IDC_COMBO_PORT, nullptr,nullptr);
    SendMessageW(G.hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- 포트 --");
    SendMessageW(G.hComboPort, CB_SETCURSEL, 0, 0);

    G.hBtnCom = CreateWindowW(L"BUTTON", L"COM OPL3 ON",
        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        390, y, 130, 38, hwnd, (HMENU)(INT_PTR)IDC_BTN_COM, nullptr,nullptr);
    y += 52;

    // 볼륨 슬라이더
    CreateWindowW(L"STATIC", L"볼륨",
        WS_CHILD|WS_VISIBLE, 20, y, 60, 20, hwnd,nullptr,nullptr,nullptr);
    G.hSliderVol = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS|TBS_TOOLTIPS,
        85, y-5, 455, 30, hwnd, (HMENU)(INT_PTR)IDC_SLIDER_VOL, nullptr,nullptr);
    SendMessageW(G.hSliderVol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(G.hSliderVol, TBM_SETPOS,   TRUE, 100);
    y += 40;

    // 게인 슬라이더
    CreateWindowW(L"STATIC", L"게인",
        WS_CHILD|WS_VISIBLE, 20, y, 60, 20, hwnd,nullptr,nullptr,nullptr);
    G.hSliderGain = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS|TBS_TOOLTIPS,
        85, y-5, 455, 30, hwnd, (HMENU)(INT_PTR)IDC_SLIDER_GAIN, nullptr,nullptr);
    SendMessageW(G.hSliderGain, TBM_SETRANGE, TRUE, MAKELPARAM(50, 400));
    SendMessageW(G.hSliderGain, TBM_SETPOS,   TRUE, 100);
    y += 45;

    // 타이밍 프리셋
    CreateWindowW(L"STATIC", L"타이밍 프리셋",
        WS_CHILD|WS_VISIBLE, 20, y, 90, 20, hwnd,nullptr,nullptr,nullptr);
    G.hPresetCombo = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        115, y-2, 260, 200, hwnd, (HMENU)(INT_PTR)IDC_PRESET, nullptr,nullptr);
    SendMessageW(G.hPresetCombo, CB_ADDSTRING, 0, (LPARAM)L"밸런스(권장)");
    SendMessageW(G.hPresetCombo, CB_ADDSTRING, 0, (LPARAM)L"지터 최소");
    SendMessageW(G.hPresetCombo, CB_ADDSTRING, 0, (LPARAM)L"누락 방지(대용량)");
    SendMessageW(G.hPresetCombo, CB_ADDSTRING, 0, (LPARAM)L"갭 보존(긴 텀 유지)");
    SendMessageW(G.hPresetCombo, CB_ADDSTRING, 0, (LPARAM)L"Raw(그대로)");
    SendMessageW(G.hPresetCombo, CB_SETCURSEL, 0, 0);
    y += 30;

    // UDP/COM 통계
    G.hUdpStats = CreateWindowW(L"STATIC",
        L"UDP: Bytes 0  Packets 0  Events 0  Drop 0  SeqGap 0",
        WS_CHILD|WS_VISIBLE, 20, y, 520, 20, hwnd,
        (HMENU)(INT_PTR)IDC_UDP_STATS, nullptr,nullptr);
    y += 30;

    // 로그
    CreateWindowW(L"STATIC", L"이벤트 로그",
        WS_CHILD|WS_VISIBLE, 20, y, 200, 20, hwnd,nullptr,nullptr,nullptr);
    y += 22;
    G.hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        20, y, 520, 160, hwnd, (HMENU)(INT_PTR)IDC_LOG, nullptr,nullptr);
    y += 175;

    // 크레딧
    HWND hC1 = CreateWindowW(L"STATIC", L"2025 전기가오리  /  네이버카페 도스박물관",
        WS_CHILD|WS_VISIBLE|SS_CENTER, 0, y, 560, 20, hwnd,nullptr,nullptr,nullptr);
    HFONT hCF = CreateFontW(13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_DONTCARE,L"맑은 고딕");
    SendMessageW(hC1, WM_SETFONT, (WPARAM)hCF, TRUE);

    HFONT hF = CreateFontW(15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_DONTCARE,L"맑은 고딕");
    EnumChildWindows(hwnd, [](HWND hc, LPARAM lp) -> BOOL {
        wchar_t cls[64]; GetClassNameW(hc, cls, 64);
        if (wcscmp(cls,L"BUTTON") && wcscmp(cls,L"LISTBOX"))
            SendMessageW(hc, WM_SETFONT, lp, TRUE);
        return TRUE;
    }, (LPARAM)hF);
}

// ------------------------------------------------------------
// WndProc
// ------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        if (!g_hBgBrush)       g_hBgBrush       = CreateSolidBrush(APPLE_BG);
        if (!g_hSliderBgBrush) g_hSliderBgBrush = CreateSolidBrush(APPLE_WHITE);
        BuildUI(hwnd);
        ApplyLivePreset(0);
        RefreshComPorts();
        return 0;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_PRESET && HIWORD(wParam) == CBN_SELCHANGE) {
            ApplyLivePreset((int)SendMessageW(G.hPresetCombo, CB_GETCURSEL, 0, 0));
            break;
        }
        if (id == IDC_BTN_SINE) {
            std::thread(TestSine).detach();
        }
        else if (id == IDC_BTN_STOP) {
            StopAllPlayback();
        }
        else if (id == IDC_BTN_RESET) {
            StopAllPlayback(); InitOPL3();
            LogLine(L"[칩] OPL3 초기화 완료");
        }
        else if (id == IDC_BTN_UDP) {
            bool isOn = g_udpReceiveEnabled.load();
            if (isOn) {
                g_udpReceiveEnabled.store(false);
                g_playFromNetwork.store(false);
                SetWindowTextW(G.hBtnUdp, L"UDP OPL3 ON");
                LogLine(L"[UDP] 수신 OFF");
            } else {
                if (g_comReceiveEnabled.load()) {
                    StopComReceive(); g_playFromNetwork.store(false);
                }
                PrepareUdpOn(); InitOPL3();
                g_udpReceiveEnabled.store(true);
                g_playFromNetwork.store(true);
                SetWindowTextW(G.hBtnUdp, L"UDP OPL3 OFF");
                LogLine(L"[UDP] 수신 ON  →  ESP32 192.168.4.1:9800  포트 9800 대기");
            }
            if (G.hLedUdp) InvalidateRect(G.hLedUdp, nullptr, TRUE);
        }
        else if (id == IDC_BTN_COM) {
            bool isOn = g_comReceiveEnabled.load();
            if (isOn) {
                StopComReceive(); g_playFromNetwork.store(false);
                LogLine(L"[COM] 수신 OFF");
            } else {
                int sel = (int)SendMessageW(G.hComboPort, CB_GETCURSEL, 0, 0);
                if (sel <= 0) {
                    LogLine(L"[COM] 오류: 포트를 먼저 선택하세요"); break;
                }
                wchar_t portName[32];
                SendMessageW(G.hComboPort, CB_GETLBTEXT, sel, (LPARAM)portName);

                if (g_udpReceiveEnabled.load()) {
                    g_udpReceiveEnabled.store(false);
                    g_playFromNetwork.store(false);
                    SetWindowTextW(G.hBtnUdp, L"UDP OPL3 ON");
                }
                if (!OpenComPort(portName)) {
                    LogLine(L"[COM] 오류: %s 열기 실패", portName); break;
                }
                ClearLiveRing(); InitOPL3();
                g_comReceiveEnabled.store(true);
                g_playFromNetwork.store(true);
                g_comBytes.store(0); g_comEvents.store(0); g_comErrors.store(0);
                if (g_comThread.joinable()) g_comThread.join();
                g_comThread = std::thread(ComReceiverThread);
                SetWindowTextW(G.hBtnCom, L"COM OPL3 OFF");
                LogLine(L"[COM] %s 열림  →  115200 baud OPL3 수신 시작", portName);
            }
        }
        break;
    }

    case WM_HSCROLL:
    {
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == G.hSliderVol) {
            double v = (double)SendMessageW(G.hSliderVol, TBM_GETPOS, 0, 0) / 100.0;
            g_userVolume.store(std::max(0.0, std::min(1.0, v)));
        }
        else if (hCtrl == G.hSliderGain) {
            double g = (double)SendMessageW(G.hSliderGain, TBM_GETPOS, 0, 0) / 100.0;
            g_userGain.store(std::max(0.5, std::min(4.0, g)));
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC  hdc   = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, APPLE_TEXT);
        if (hCtrl == G.hSliderVol || hCtrl == G.hSliderGain)
            return (LRESULT)g_hSliderBgBrush;
        return (LRESULT)g_hBgBrush;
    }

    case WM_DRAWITEM:
        HandleDrawItem((LPDRAWITEMSTRUCT)lParam);
        return TRUE;

    case WM_TIMER:
        if (wParam == 1) {
            if (G.hLedUdp) InvalidateRect(G.hLedUdp, nullptr, TRUE);
            if (G.hUdpStats) {
                wchar_t s[320];
                swprintf_s(s,
                    L"UDP: Pkt %llu  Ev %llu  Drop %llu  Gap %llu  |  COM: Ev %llu  Err %llu  Bytes %llu",
                    (ULONGLONG)g_udpPackets.load(), (ULONGLONG)g_udpEvents.load(),
                    (ULONGLONG)g_udpDrops.load(),   (ULONGLONG)g_udpSeqGaps.load(),
                    (ULONGLONG)g_comEvents.load(),  (ULONGLONG)g_comErrors.load(),
                    (ULONGLONG)g_comBytes.load());
                SetWindowTextW(G.hUdpStats, s);
            }
        }
        return 0;

    case WM_DESTROY:
        g_udpReceiveEnabled.store(false);
        g_playFromNetwork.store(false);
        g_comReceiveEnabled.store(false);
        g_appQuit.store(true);
        if (g_comThread.joinable()) g_comThread.join();
        if (g_hCom != INVALID_HANDLE_VALUE) { CloseHandle(g_hCom); g_hCom = INVALID_HANDLE_VALUE; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ------------------------------------------------------------
// wWinMain
// ------------------------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_WIN95_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&ic);

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        MessageBoxA(nullptr, Pa_GetErrorText(err), "PortAudio Error", MB_ICONERROR);
        return 0;
    }

    InitOPL3();

    PaStreamParameters out{};
    out.device           = Pa_GetDefaultOutputDevice();
    out.channelCount     = 2;
    out.sampleFormat     = paFloat32;
    out.suggestedLatency = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;

    err = Pa_OpenStream(&g_paStream, nullptr, &out,
        49716.0, kFramesPerBuffer, paNoFlag, PaCallback, nullptr);
    if (err != paNoError) {
        err = Pa_OpenStream(&g_paStream, nullptr, &out,
            48000.0, kFramesPerBuffer, paNoFlag, PaCallback, nullptr);
        if (err != paNoError) {
            err = Pa_OpenStream(&g_paStream, nullptr, &out,
                44100.0, kFramesPerBuffer, paNoFlag, PaCallback, nullptr);
        }
    }
    if (err != paNoError) {
        MessageBoxA(nullptr, Pa_GetErrorText(err), "PortAudio Error", MB_ICONERROR);
        Pa_Terminate(); return 0;
    }

    const PaStreamInfo* si = Pa_GetStreamInfo(g_paStream);
    if (si) kSampleRate = si->sampleRate;
    Pa_StartStream(g_paStream);

    g_udpThread = std::thread(UdpReceiverThread);

    if (!g_hBgBrush) g_hBgBrush = CreateSolidBrush(APPLE_BG);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"OPLPLAYER";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hBgBrush;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName,
        L"OPLplayer  [adlcom ElectricRay]",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 700,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 100, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_appQuit.store(true);
    if (g_comThread.joinable()) g_comThread.join();
    if (g_udpThread.joinable()) g_udpThread.join();

    if (g_paStream) { Pa_StopStream(g_paStream); Pa_CloseStream(g_paStream); }
    Pa_Terminate();
    if (g_hBgBrush)       DeleteObject(g_hBgBrush);
    if (g_hSliderBgBrush) DeleteObject(g_hSliderBgBrush);
    return 0;
}

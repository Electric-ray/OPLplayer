// opl_engine.cpp  – OPLplayer Android NDK 코어
//  Windows OPLplayer.cpp에서 이식:
//    Winsock  → POSIX 소켓
//    PortAudio → Oboe AudioStreamDataCallback
//    Win32 스레드 → std::thread
//    COM 포트 직접 열기 제거 → JNI feedSerial() 로 데이터 수신
// ---------------------------------------------------------------
#include "opl_engine.h"

#include <oboe/Oboe.h>
#include <android/log.h>

#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <thread>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>

#include "ymfm_opl.h"

#define TAG "OPLplayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------------------------------------------------------------
// 오디오 설정
// ---------------------------------------------------------------
static double              g_sampleRate    = 49716.0;
static const int           kFramesPerBurst = 256;

// ---------------------------------------------------------------
// YMFM OPL3
// ---------------------------------------------------------------
class YmfmInterface final : public ymfm::ymfm_interface {
public:
    void    ymfm_set_timer(uint32_t, int32_t) override {}
    void    ymfm_set_busy_end(uint32_t) override {}
    bool    ymfm_is_busy() override { return false; }
    void    ymfm_update_irq(bool) override {}
    uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
    void    ymfm_external_write(ymfm::access_class, uint32_t, uint8_t) override {}
};

static YmfmInterface  g_intf;
static ymfm::ymf262   g_chip(g_intf);

static void WriteOPL3Reg(uint16_t reg, uint8_t val) {
    uint8_t  addr  = (uint8_t)(reg & 0xFF);
    // OPL2 호환: 0xC0-0xC8 패닝 비트 없으면 양쪽 출력 강제 (IMPlay 등)
    if (addr >= 0xC0 && addr <= 0xC8 && (val & 0x30) == 0) val |= 0x30;
    uint8_t  bank  = (uint8_t)((reg >> 8) & 1);
    uint32_t port  = (uint32_t)(bank * 2);
    g_chip.write(port,     addr);
    g_chip.write(port + 1, val);
}

void OplEngine_InitChip() {
    g_chip.reset();
    g_chip.write(2, 0x05);
    g_chip.write(3, 0x01);
    WriteOPL3Reg(0x001, 0x20);
    for (int i = 0; i < 9; ++i) {
        WriteOPL3Reg((uint16_t)(0x0B0 + i), 0x00);
        WriteOPL3Reg((uint16_t)(0x1B0 + i), 0x00);
    }
    LOGI("OPL3 chip reset");
}

// ---------------------------------------------------------------
// 라이브 이벤트 링버퍼  (Windows 버전과 동일)
// ---------------------------------------------------------------
struct LiveEvent { uint64_t sample; uint16_t reg; uint8_t val; };

static const uint32_t        kRingSize = 1u << 17;
static LiveEvent             g_ring[kRingSize];
static std::atomic<uint32_t> g_ringW(0), g_ringR(0);

static std::atomic<uint64_t> g_playhead(0);
static std::atomic<uint64_t> g_cursor(0);
static std::atomic<bool>     g_playFromNet(false);

static std::atomic<uint64_t> g_udpPackets(0), g_udpEvents(0), g_udpDrops(0);
static std::atomic<uint64_t> g_comEvents(0),  g_comErrors(0);

static void PushLiveEvent(uint64_t sample, uint16_t reg, uint8_t val) {
    uint32_t w  = g_ringW.load(std::memory_order_relaxed);
    uint32_t r  = g_ringR.load(std::memory_order_acquire);
    uint32_t nw = (w + 1) & (kRingSize - 1);
    if (nw == r) { g_udpDrops.fetch_add(1, std::memory_order_relaxed); return; }
    g_ring[w] = { sample, reg, val };
    g_ringW.store(nw, std::memory_order_release);
}

static void ClearRing() {
    g_ringR.store(g_ringW.load(std::memory_order_relaxed), std::memory_order_release);
}

static void SafeResetCursor(uint64_t prebufSamples) {
    ClearRing();
    g_cursor.store(g_playhead.load(std::memory_order_relaxed) + prebufSamples,
                   std::memory_order_relaxed);
}

// ---------------------------------------------------------------
// Nuke.YKT 3바이트 디코더 (USB 시리얼용)
// ---------------------------------------------------------------
class NukeDecoder {
public:
    enum class S { Wait, Got0, Got1 };
    void reset() { st=S::Wait; hb=false; reg=0; val=0; errs=0; pkts=0; }
    bool feed(uint8_t b, uint16_t& outReg, uint8_t& outVal) {
        if (b & 0x80) {
            if (st != S::Wait) errs++;
            uint8_t s = b & 0x0F;
            hb  = (s >= 4);
            reg = (s & 3) << 6;
            st  = S::Got0; return false;
        }
        switch (st) {
        case S::Wait: break;
        case S::Got0:
            reg |= (b >> 1) & 0x3F;
            val  = (b & 1) << 7;
            st = S::Got1; break;
        case S::Got1:
            val |= b & 0x7F;
            outReg = reg | (hb ? 0x100u : 0u);
            outVal = val; pkts++;
            st = S::Wait; return true;
        }
        return false;
    }
    uint64_t errs=0, pkts=0;
private:
    S st=S::Wait; bool hb=false; uint8_t reg=0, val=0;
};

// ---------------------------------------------------------------
// Oboe 오디오 콜백
// ---------------------------------------------------------------
static std::atomic<float> g_volume(1.0f), g_gain(1.0f);
static const float kMasterGain = 0.5f;

class OplAudioCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* /*stream*/,
            void* audioData,
            int32_t numFrames) override
    {
        float* out = (float*)audioData;
        for (int i = 0; i < numFrames; ++i) {
            uint64_t cur = g_playhead.fetch_add(1, std::memory_order_relaxed);
            if (g_playFromNet.load(std::memory_order_relaxed)) {
                uint32_t r = g_ringR.load(std::memory_order_relaxed);
                uint32_t w = g_ringW.load(std::memory_order_acquire);
                while (r != w) {
                    if (g_ring[r].sample > cur) break;
                        WriteOPL3Reg(g_ring[r].reg, g_ring[r].val);
                    r = (r + 1) & (kRingSize - 1);
                }
                g_ringR.store(r, std::memory_order_release);
            }
        ymfm::ymf262::output_data od{};
        g_chip.generate(&od);
            float L = (float)od.data[0] + (float)od.data[2];
            float R = (float)od.data[1] + (float)od.data[3];
            float scale = kMasterGain * g_volume.load() * g_gain.load() / 32768.f;
            auto clip = [](float x){ return x>1.f?1.f:(x<-1.f?-1.f:x); };
            *out++ = clip(L * scale);
            *out++ = clip(R * scale);
        }
        return oboe::DataCallbackResult::Continue;
    }
};

static OplAudioCallback              g_audioCb;
static std::shared_ptr<oboe::AudioStream> g_stream;

static bool StartAudio() {
    oboe::AudioStreamBuilder b;
    b.setChannelCount(2)
     ->setSampleRate((int)g_sampleRate)
     ->setFormat(oboe::AudioFormat::Float)
     ->setDataCallback(&g_audioCb)
     ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
     ->setSharingMode(oboe::SharingMode::Exclusive)
     ->setUsage(oboe::Usage::Game)
     ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);

    oboe::Result r = b.openStream(g_stream);
    if (r != oboe::Result::OK) {
        LOGE("Oboe openStream failed: %s", oboe::convertToText(r));
        return false;
    }
    int32_t actualRate = g_stream->getSampleRate();
    g_sampleRate = (double)actualRate;
    LOGI("Oboe rate: actual=%d Hz  OPL3-native=49716 Hz  diff=%.1f%%  burst=%d",
         actualRate, (49716.0-actualRate)/49716.0*100.0, g_stream->getFramesPerBurst());
    g_stream->requestStart();
    return true;
}

static void StopAudio() {
    if (g_stream) { g_stream->stop(); g_stream->close(); g_stream.reset(); }
}

// ---------------------------------------------------------------
// UDP 수신 스레드 (POSIX 소켓)  - Windows 버전과 동일 로직
// ---------------------------------------------------------------
static std::atomic<bool>     g_udpEnabled(false);
static std::atomic<uint64_t> g_udpStreamClock(0); // UDP 전용 누적 스트림 클럭
static std::atomic<int>      g_prebufMs(50);

// UDP 패킷 파싱 - 소켓/네트워크는 Kotlin UdpReceiver 담당
// Kotlin이 수신한 raw 패킷 바이트를 그대로 전달받아 처리
// UDP 패킷 파싱 - 소켓/네트워크는 Kotlin UdpReceiver 담당
// UDP 패킷 파싱 - 스트림 클럭 방식으로 DOS 원본 타이밍 보존
// note-on/note-off가 패킷 경계에 걸려도 정확한 음 길이 유지
void OplEngine_FeedUdpPacket(const uint8_t* data, int len) {
    if (len < 8) return;
    if (data[0]!=0x4F||data[1]!=0x50||data[2]!=0x4C||data[3]!=0x33) return;

    g_udpPackets.fetch_add(1, std::memory_order_relaxed);

    uint8_t  type  = data[4];
    uint8_t  count = data[5];
    uint16_t seq   = (uint16_t)(data[6] | (data[7]<<8));

    static bool     haveSeq = false;
    static uint16_t lastSeq = 0;

    if (type == 1) {
        haveSeq = false; ClearRing();
        uint64_t pb = (uint64_t)(g_sampleRate * g_prebufMs.load() / 1000.0);
        g_udpStreamClock.store(g_playhead.load(std::memory_order_relaxed) + pb);
        return;
    }
    if (type != 0 || count == 0) return;
    if (!g_udpEnabled.load(std::memory_order_relaxed)) return;

    if (haveSeq) {
        if (seq == lastSeq) return;
        uint16_t diff = (uint16_t)(seq - lastSeq);
        if (diff & 0x8000) return;
        if (diff != 1) {
            ClearRing(); haveSeq = false;
            uint64_t pb = (uint64_t)(g_sampleRate * g_prebufMs.load() / 1000.0);
            g_udpStreamClock.store(g_playhead.load(std::memory_order_relaxed) + pb);
        }
    }
    haveSeq = true; lastSeq = seq;

    const int      pbMs    = std::max(20, std::min(200, (int)g_prebufMs.load()));
    const uint64_t kPrebuf = (uint64_t)(g_sampleRate * pbMs / 1000.0);
    const uint64_t kMax    = kPrebuf + (uint64_t)(g_sampleRate * 0.3);

    uint64_t ph  = g_playhead.load(std::memory_order_relaxed);
    uint64_t clk = g_udpStreamClock.load(std::memory_order_relaxed);

    // 너무 앞서면 ring flush + 리셋
    if (clk > ph + kMax) {
        SafeResetCursor(kPrebuf);
        ph  = g_playhead.load(std::memory_order_relaxed);
        clk = ph + kPrebuf;
    }
    // 너무 뒤처지면 prebuf 위치로 catch-up
    if (clk < ph + kPrebuf) clk = ph + kPrebuf;

    // deltaUs 누적으로 DOS 원본 타이밍 보존
    uint64_t prevClk = clk;
    int off = 8;
    for (int i = 0; i < (int)count; ++i) {
        if (off + 6 > len) break;
        uint16_t deltaUs = (uint16_t)(data[off+0] | (data[off+1]<<8));
        uint16_t reg     = (uint16_t)(data[off+2] | (data[off+3]<<8));
        uint8_t  val     = data[off+4];
        off += 6;
        uint8_t  bank    = (uint8_t)((reg >> 8) & 1);
        uint16_t regFull = (uint16_t)((reg & 0xFF) | (bank ? 0x100u : 0u));
        clk += (uint64_t)((double)deltaUs * g_sampleRate / 1e6);
        if (clk <= prevClk) clk = prevClk + 1;
        prevClk = clk;
        PushLiveEvent(clk, regFull, val);
        g_udpEvents.fetch_add(1, std::memory_order_relaxed);
    }
    // 스트림 클럭 업데이트 (패킷 경계를 넘어 누적)
    g_udpStreamClock.store(clk, std::memory_order_relaxed);
}


// ---------------------------------------------------------------
// USB 시리얼 데이터 수신 (Kotlin에서 feedSerial 호출)
// ---------------------------------------------------------------
static NukeDecoder g_nukeDecoder;
static std::mutex  g_decoderMutex;
static std::atomic<bool> g_serialEnabled(false);

void OplEngine_FeedSerial(const uint8_t* data, int len) {
    if (!g_serialEnabled.load() || len <= 0) return;
    const uint64_t kPrebuf = (uint64_t)(g_sampleRate * 30.0 / 1000.0); // 30ms
    const uint64_t kMax    = kPrebuf + (uint64_t)(g_sampleRate * 0.3);
    const uint64_t kStep   = 13; // 115200bps 물리 한계

    uint64_t ph  = g_playhead.load(std::memory_order_relaxed);
    uint64_t cur = g_cursor.load(std::memory_order_relaxed);
    if (cur > ph + kMax) {
        SafeResetCursor(kPrebuf);
        ph  = g_playhead.load(std::memory_order_relaxed);
        cur = g_cursor.load(std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lk(g_decoderMutex);
    for (int i = 0; i < len; ++i) {
        uint16_t reg; uint8_t val;
        if (g_nukeDecoder.feed((uint8_t)data[i], reg, val)) {
            uint64_t next = cur + kStep;
            if (next < ph + kPrebuf) next = ph + kPrebuf;
            cur = next;
            g_cursor.store(cur, std::memory_order_relaxed);
            PushLiveEvent(cur, reg, val);
            g_comEvents.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_comErrors.store(g_nukeDecoder.errs, std::memory_order_relaxed);
}

// ---------------------------------------------------------------
// 공개 API  (opl_engine.h에 선언)
// ---------------------------------------------------------------
bool OplEngine_Start() {
    OplEngine_InitChip();
    g_playhead.store(0); g_cursor.store(0);
    ClearRing();
    g_nukeDecoder.reset();
    if (!StartAudio()) return false;    LOGI("OplEngine started, sampleRate=%.0f", g_sampleRate);
    return true;
}

void OplEngine_Stop() {
    g_udpEnabled.store(false);
    g_serialEnabled.store(false);
    g_playFromNet.store(false);    StopAudio();
    LOGI("OplEngine stopped");
}

void OplEngine_SetUdpEnabled(bool en) {
    if (en) { ClearRing(); g_cursor.store(0); g_nukeDecoder.reset(); }
    g_serialEnabled.store(false);
    g_udpEnabled.store(en);
    g_playFromNet.store(en);
    LOGI("UDP %s", en ? "ON" : "OFF");
}

void OplEngine_SetSerialEnabled(bool en) {
    if (en) { ClearRing(); g_cursor.store(0); g_nukeDecoder.reset(); }
    g_udpEnabled.store(false);
    g_serialEnabled.store(en);
    g_playFromNet.store(en);
    LOGI("Serial %s", en ? "ON" : "OFF");
}

void OplEngine_SetVolume(float v) { g_volume.store(std::max(0.f, std::min(1.f, v))); }
void OplEngine_SetGain(float g)   { g_gain.store(std::max(0.5f, std::min(4.f, g))); }
void OplEngine_SetPrebufMs(int ms){ g_prebufMs.store(std::max(20, std::min(200, ms))); }

std::string OplEngine_GetStats() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "UDP pkt:%llu ev:%llu drop:%llu | Serial ev:%llu err:%llu",
        (unsigned long long)g_udpPackets.load(),
        (unsigned long long)g_udpEvents.load(),
        (unsigned long long)g_udpDrops.load(),
        (unsigned long long)g_comEvents.load(),
        (unsigned long long)g_comErrors.load());
    return std::string(buf);
}

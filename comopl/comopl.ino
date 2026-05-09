// ============================================================================
// ESP32-S3 OPL Timing Bridge (AP mode, no Serial logs)
// UART (NukeYKT) -> UDP 9800 ("OPL3" timed batch packets)
// Packet format (little-endian):
//   magic[4] = "OPL3"
//   type     = 0 events, 1 reset
//   count    = 0..32
//   seq      = uint16
//   events[count]:
//     deltaUs uint16   (time since previous event; clamped to 65535)
//     reg     uint16   (bank<<8 | reg8) ; bank is normalized to 0/1
//     val     uint8
//     flags   uint8    (0)
// ============================================================================

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_timer.h>
#include <cstring>

// ---------------- AP config ----------------
static const char* AP_SSID = "COMOPL";
static const char* AP_PASS = "12345678"; // >= 8 chars for WPA2

// PC side: usually gets 192.168.4.2 when it connects to ESP32 AP.
// Broadcast is simplest (works even if IP changes).
static const IPAddress PC_IP(192, 168, 4, 255);
static const uint16_t  UDP_PORT = 9800;

// ---------------- UART config ----------------
static const int SERIAL_BAUD   = 115200;
static const int SERIAL_RX_PIN = 44;  // as requested
static const int SERIAL_TX_PIN = -1;  // not used

// ---------------- batching ----------------
static const uint8_t  MAX_EVENTS_PER_BATCH = 32;
static const uint32_t BATCH_TIMEOUT_US     = 1500; // flush if no new events for 1.5ms

// ---------------- LED (optional) ----------------
static const int LED_PIN = 2;

// ---------------- packet structs ----------------
#pragma pack(push, 1)
struct TimedOPLEvent {
  uint16_t deltaUs;
  uint16_t reg;
  uint8_t  val;
  uint8_t  flags;
};

struct OPLEventBatch {
  uint8_t magic[4];     // 'O','P','L','3'
  uint8_t type;         // 0=events, 1=reset
  uint8_t count;        // number of events
  uint16_t seq;         // packet sequence
  TimedOPLEvent events[MAX_EVENTS_PER_BATCH];
};
#pragma pack(pop)

static WiFiUDP udp;

static IPAddress g_targetIp = PC_IP;
static uint16_t  g_targetPort = UDP_PORT;
static bool      g_haveTarget = false;

// 0이면 PC에서 HELLO를 받아야만 전송(유니캐스트, 손실 최소화)
// 1이면 HELLO 전에는 브로드캐스트(PC_IP)로 전송(손실 가능성↑)
static const bool USE_BROADCAST_FALLBACK = false;

static void PollHello()
{
    int p = udp.parsePacket();
    if (p <= 0) return;

    char msg[8];
    int n = udp.read((uint8_t*)msg, (p > 7) ? 7 : p);
    if (n <= 0) return;
    msg[n] = 0;

    if (strcmp(msg, "HELLO") == 0)
    {
        g_targetIp = udp.remoteIP();
        g_targetPort = UDP_PORT; // 고정 (PC는 9800 bind)
        g_haveTarget = true;
    }
}

static OPLEventBatch batch;

static uint16_t g_seq = 0;

// NukeYKT decoder state
static int     st = 0;
static uint8_t tempBank = 0;
static uint8_t tempReg  = 0;
static uint8_t tempVal  = 0;

// timing
static uint64_t lastEventUs = 0;
static uint64_t lastFlushUs = 0;
static bool     haveFirst   = false;

static inline uint16_t clampU16(uint64_t x) {
  return (x > 65535ULL) ? 65535 : (uint16_t)x;
}

static void initBatch(uint8_t type = 0) {
  batch.magic[0] = 'O';
  batch.magic[1] = 'P';
  batch.magic[2] = 'L';
  batch.magic[3] = '3';
  batch.type  = type;
  batch.count = 0;
  batch.seq   = g_seq;
}

static void sendBatch() {
  if (batch.type == 0 && batch.count == 0) return;

  if (!g_haveTarget && !USE_BROADCAST_FALLBACK) {
    // 아직 PC를 못 찾았으면 버려서 링이 쌓이지 않게 함
    initBatch(0);
    lastFlushUs = esp_timer_get_time();
    return;
  }

  const size_t headerSize = 8; // magic[4]+type+count+seq(2)
  const size_t pktSize    = headerSize + (size_t)batch.count * sizeof(TimedOPLEvent);

  const IPAddress& ip = g_haveTarget ? g_targetIp : PC_IP;
  const uint16_t   port = g_haveTarget ? g_targetPort : UDP_PORT;

  udp.beginPacket(ip, port);
  udp.write((const uint8_t*)&batch, pktSize);
  udp.endPacket();

  g_seq++;
  initBatch(0);
  lastFlushUs = esp_timer_get_time();
}
static void sendResetPacket() {
  initBatch(1);
  batch.count = 0;
  sendBatch();
}

static void addEvent(uint16_t deltaUs, uint16_t reg, uint8_t val) {
  if (batch.type != 0) {
    initBatch(0);
  }

  if (batch.count >= MAX_EVENTS_PER_BATCH) {
    sendBatch();
  }

  TimedOPLEvent &e = batch.events[batch.count++];
  e.deltaUs = deltaUs;
  e.reg     = reg;
  e.val     = val;
  e.flags   = 0;

  // flush aggressively if a large gap appears (helps phrase boundaries)
  if (batch.count >= MAX_EVENTS_PER_BATCH || deltaUs > 10000) {
    sendBatch();
  }
}

static void processByte(uint8_t b) {
  const uint64_t now = esp_timer_get_time();

  if (st == 0) {
    if (b & 0x80) {
      tempBank = (b >> 2) & 0x03;
      tempReg  = (b & 0x03) << 6;
      st = 1;
    }
  }
  else if (st == 1) {
      if (b & 0x80) { // 예상치 못한 CMD → 파서 리셋
        tempBank = (b >> 2) & 0x03;
        tempReg  = (b & 0x03) << 6;
        // st = 1 유지 (새 CMD로 처리)
        return;
    }
    tempReg |= (b >> 1) & 0x3F;
    tempVal  = (b & 0x01) << 7;
    st = 2;
  }
  else { // st==2
    tempVal |= b & 0x7F;

    // normalize bank to 0/1 (PC side maps bank!=0 -> 0x100)
    const uint16_t bank01  = (tempBank ? 1 : 0);
    const uint16_t fullReg = (uint16_t)((bank01 << 8) | tempReg);

    uint16_t deltaUs = 0;
    if (!haveFirst) {
      haveFirst = true;
      deltaUs = 0;
    } else {
      deltaUs = clampU16(now - lastEventUs);
    }
    lastEventUs = now;

    addEvent(deltaUs, fullReg, tempVal);

    st = 0;
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // start UDP (no need udp.begin for sending only, but harmless)
  udp.begin(UDP_PORT);

  // UART (RX only)
  Serial1.begin(SERIAL_BAUD, SERIAL_8N1, SERIAL_RX_PIN, SERIAL_TX_PIN);
  Serial1.setRxBufferSize(4096);

  initBatch(0);
  lastFlushUs = esp_timer_get_time();
  lastEventUs = lastFlushUs;

  // simple "connected" LED (AP up)
  digitalWrite(LED_PIN, HIGH);

  // optional: tell PC to reset timeline
  sendResetPacket();
}

void loop() {
  PollHello();
  // pull UART bytes
  while (Serial1.available()) {
    const uint8_t b = (uint8_t)Serial1.read();
    processByte(b);
  }

  // flush by timeout
  if (batch.type == 0 && batch.count > 0) {
    const uint64_t now = esp_timer_get_time();
    if (now - lastFlushUs > BATCH_TIMEOUT_US) {
      sendBatch();
    }
  }

  // tiny yield
  delayMicroseconds(10);
}

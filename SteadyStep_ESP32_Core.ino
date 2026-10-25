/*
  SteadyStep — ESP32 Core Firmware (provisional hardware integration)
  Target: ESP32-WROOM-32 dev board, Arduino-ESP32 core

  Includes:
    - two HX711 channels (platform force and grab-bar support force)
    - experimental BCG heart-rate estimation from platform HX711 raw samples
    - raw ADC logging for the impedance analogue front end (NOT converted to ohms yet)
    - baseline collection and threshold/persistence rules
    - LittleFS CSV history
    - DFPlayer Mini prerecorded voice prompts
    - SIM800L SMS with queued retry behavior

  IMPORTANT:
    Pin mapping and sensor mapping are assumptions because the supplied schematic does
    not yet unambiguously show all ESP32 connections or a complete impedance excitation
    circuit. Do not connect electrodes to a person until the analogue front end has been
    independently reviewed for safe current limiting, isolation/protection, and correct
    excitation. Do not treat this as a medical device.
*/

#include <Arduino.h>
#include <HX711.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

// ------------------------- PROVISIONAL PIN MAP -------------------------
// Verify and change these against the final EasyEDA schematic before wiring.
static constexpr int PIN_FLOOR_DOUT = 32;
static constexpr int PIN_FLOOR_SCK  = 33;
static constexpr int PIN_BAR_DOUT   = 18;
static constexpr int PIN_BAR_SCK    = 19;
static constexpr int PIN_IMPEDANCE_ADC = 34;  // conditioned DC/envelope only; ADC max <= 3.3 V

static constexpr int PIN_DFPLAYER_RX = 16;    // ESP32 RX <- DFPlayer TX
static constexpr int PIN_DFPLAYER_TX = 17;    // ESP32 TX -> DFPlayer RX (series resistor recommended)
static constexpr int PIN_SIM800_RX   = 26;    // ESP32 RX <- SIM800L TX
static constexpr int PIN_SIM800_TX   = 27;    // ESP32 TX -> SIM800L RX; level compatibility must be checked

// -------------------------- DEMO PARAMETERS ----------------------------
static constexpr char DEVICE_ID[] = "SS001";
static constexpr uint32_t SESSION_LENGTH_MS = 10000;
static constexpr uint32_t AUTO_PRESENCE_STABLE_MS = 700;
static constexpr float PRESENCE_FORCE_KG_EQ = 15.0f;
static constexpr uint32_t AUTO_COOLDOWN_MS = 20UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t GSM_RETRY_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t GSM_INIT_RETRY_MS = 30UL * 1000UL;
static constexpr uint32_t MODEM_CLOCK_REFRESH_MS = 6UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t IMPEDANCE_SAMPLE_PERIOD_MS = 10;
static constexpr int MAX_FLOOR_SAMPLES = 1000;
static constexpr int MAX_BAR_SAMPLES = 1000;
static constexpr int MAX_ADC_SAMPLES = 1100;
static constexpr uint8_t BASELINE_TARGET = 7;
static constexpr float WEIGHT_RISE_THRESHOLD_PCT = 2.0f; // engineering placeholder, not a clinical threshold
static constexpr float HR_RISE_THRESHOLD_PCT = 15.0f;    // engineering placeholder, not a clinical threshold
static constexpr uint32_t LOG_ROTATE_AT_BYTES = 420UL * 1024UL;
static constexpr uint16_t LOG_RETAIN_ROWS = 900;

// Must remain false until the actual excitation/analogue circuit is verified and calibrated.
// The firmware logs ADC counts only; it does not currently compute impedance in ohms or use
// impedance to trigger alerts.
static constexpr bool IMPEDANCE_FRONT_END_VERIFIED = false;

// ------------------------------- DEVICES --------------------------------
HX711 floorHx;
HX711 barHx;
HardwareSerial DFSerial(1);
HardwareSerial GSMSerial(2);
DFRobotDFPlayerMini player;
Preferences prefs;

// ------------------------------- DATA -----------------------------------
struct LoadCellCalibration {
  int32_t offset;
  float countsPerKg;
};

struct BaselineData {
  uint8_t version;
  uint8_t weightN;
  uint8_t heartN;
  uint8_t weightTrendN;
  uint8_t heartTrendN;
  uint8_t weightFlags;       // 3-bit recent history; 1 means threshold exceeded
  uint8_t heartFlags;
  uint8_t lastNotifiedState; // 0 none, 4 WATCH, 5 ELEVATED_PATTERN
  uint32_t sessions;
  int64_t lastSessionEpoch;
  float weights[BASELINE_TARGET];
  float heartRates[BASELINE_TARGET];
};

struct SessionRecord {
  uint32_t sequence = 0;
  uint32_t uptimeMs = 0;
  String timestampUtc = "UNSET";
  float platformKg = NAN;
  float grabBarKgEq = NAN;
  float trueWeightKg = NAN;
  bool weightValid = false;
  float heartRateBpm = NAN;
  bool heartRateValid = false;
  float heartRateCv = NAN;
  uint16_t floorCount = 0;
  uint16_t barCount = 0;
  uint16_t adcCount = 0;
  float impedanceAdcMean = NAN;
  float impedanceAdcStd = NAN;
  bool impedancePathVerified = IMPEDANCE_FRONT_END_VERIFIED;
  String state = "INSUFFICIENT_DATA";
  String reason = "Not evaluated";
  bool weightThreshold = false;
  bool heartThreshold = false;
};

// Forward declarations are explicit so this same source can be built by PlatformIO too.
void finishCapture();
void printSessionSummary(const SessionRecord &r);

LoadCellCalibration floorCal{0, 0.0f};
LoadCellCalibration barCal{0, 0.0f};
BaselineData baseline{};
String familyPhone;
bool littleFsReady = false;
bool audioReady = false;
bool gsmResponding = false;
bool clockValid = false;
bool captureActive = false;
bool manualCapture = false;
bool autoCaptureDoneThisBoot = false;
bool presenceTiming = false;
bool smsPending = false;
uint8_t pendingSmsState = 0;
String pendingSmsText;
uint32_t captureStartMs = 0;
uint32_t lastAdcSampleMs = 0;
uint32_t presenceStartMs = 0;
uint32_t lastGsmTryMs = 0;
uint32_t lastGsmInitTryMs = 0;
uint32_t lastClockRefreshMs = 0;
uint32_t lastSmsTryMs = 0;
uint32_t nextAutoCaptureMs = 0;
long lastFloorRaw = 0;
long lastBarRaw = 0;
uint32_t lastFloorReadMs = 0;
uint32_t lastBarReadMs = 0;
uint16_t floorN = 0, barN = 0, adcN = 0;
int32_t floorRawBuf[MAX_FLOOR_SAMPLES];
uint16_t floorTimeBuf[MAX_FLOOR_SAMPLES];
int32_t barRawBuf[MAX_BAR_SAMPLES];
uint16_t barTimeBuf[MAX_BAR_SAMPLES];
uint16_t adcBuf[MAX_ADC_SAMPLES];
uint16_t adcTimeBuf[MAX_ADC_SAMPLES];
uint32_t totalSamplesDropped = 0;
uint32_t lastBaselineMs = 0;

static constexpr uint8_t STATE_NONE = 0;
static constexpr uint8_t STATE_CALIBRATING = 1;
static constexpr uint8_t STATE_INSUFFICIENT = 2;
static constexpr uint8_t STATE_NO_RULE_TRIGGER = 3;
static constexpr uint8_t STATE_WATCH = 4;
static constexpr uint8_t STATE_ELEVATED_PATTERN = 5;

// ---------------------------- SMALL HELPERS -----------------------------
bool validCalibration(const LoadCellCalibration &c) {
  return isfinite(c.countsPerKg) && fabsf(c.countsPerKg) > 1.0f;
}

float rawToKg(long raw, const LoadCellCalibration &c) {
  if (!validCalibration(c)) return NAN;
  return ((float)raw - (float)c.offset) / c.countsPerKg;
}

uint8_t bitCount3(uint8_t v) {
  v &= 0x07;
  return (uint8_t)((v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1));
}

float medianOf(float *values, int count) {
  if (count <= 0) return NAN;
  for (int i = 1; i < count; ++i) {
    float key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) { values[j + 1] = values[j]; --j; }
    values[j + 1] = key;
  }
  if (count % 2) return values[count / 2];
  return (values[count / 2 - 1] + values[count / 2]) * 0.5f;
}

String jsonSafeCsv(String s) {
  s.replace(',', ';');
  s.replace('\r', ' ');
  s.replace('\n', ' ');
  return s;
}

String utcTimestamp() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "UNSET"; // clock unavailable or implausible
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmNow);
  return String(buffer);
}

// Convert civil date into days since 1970-01-01. Used to avoid relying on non-portable timegm().
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// ---------------------------- PERSISTENCE --------------------------------
void saveCalibration(const char *keyPrefix, const LoadCellCalibration &c) {
  String keyOffset = String(keyPrefix) + "_off";
  String keyScale = String(keyPrefix) + "_sc";
  prefs.putLong(keyOffset.c_str(), (long)c.offset);
  prefs.putFloat(keyScale.c_str(), c.countsPerKg);
}

void loadCalibration(const char *keyPrefix, LoadCellCalibration &c) {
  String keyOffset = String(keyPrefix) + "_off";
  String keyScale = String(keyPrefix) + "_sc";
  c.offset = (int32_t)prefs.getLong(keyOffset.c_str(), 0);
  c.countsPerKg = prefs.getFloat(keyScale.c_str(), 0.0f);
}

void saveBaseline() {
  baseline.version = 1;
  prefs.putBytes("base", &baseline, sizeof(baseline));
}

void loadBaseline() {
  memset(&baseline, 0, sizeof(baseline));
  size_t stored = prefs.getBytesLength("base");
  if (stored == sizeof(baseline)) prefs.getBytes("base", &baseline, sizeof(baseline));
  if (baseline.version != 1) {
    memset(&baseline, 0, sizeof(baseline));
    baseline.version = 1;
    baseline.lastNotifiedState = STATE_NONE;
    saveBaseline();
  }
}

void savePendingSms() {
  prefs.putBool("sms_pend", smsPending);
  prefs.putUChar("sms_state", pendingSmsState);
  prefs.putString("sms_text", pendingSmsText);
}

void loadPendingSms() {
  smsPending = prefs.getBool("sms_pend", false);
  pendingSmsState = prefs.getUChar("sms_state", STATE_NONE);
  pendingSmsText = prefs.getString("sms_text", "");
  if (pendingSmsText.length() == 0) smsPending = false;
}

const char *LOG_PATH = "/sessions.csv";
const char *LOG_TMP_PATH = "/sessions.tmp";
const char *LOG_HEADER = "sequence,timestamp_utc,uptime_ms,device_id,platform_force_kg,grabbar_force_kg_eq,estimated_true_weight_kg,weight_valid,heart_rate_bpm,heart_rate_valid,heart_rate_interval_cv,impedance_adc_mean,impedance_adc_std,impedance_ohm,impedance_quality,alert_state,reason,floor_samples,bar_samples,adc_samples,weight_threshold,heart_threshold";

void ensureLogFile() {
  if (!littleFsReady) return;
  if (!LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, FILE_WRITE);
    if (f) { f.println(LOG_HEADER); f.close(); }
  }
}

void rotateLogIfNeeded() {
  if (!littleFsReady || !LittleFS.exists(LOG_PATH)) return;
  File oldFile = LittleFS.open(LOG_PATH, FILE_READ);
  if (!oldFile) return;
  size_t size = oldFile.size();
  oldFile.close();
  if (size < LOG_ROTATE_AT_BYTES) return;

  // Count data rows without holding the entire log in SRAM.
  int32_t rows = 0;
  File src = LittleFS.open(LOG_PATH, FILE_READ);
  if (!src) return;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (line.length() && line != LOG_HEADER) ++rows;
  }
  src.close();
  if (rows <= LOG_RETAIN_ROWS) return;

  int32_t skipRows = rows - LOG_RETAIN_ROWS;
  src = LittleFS.open(LOG_PATH, FILE_READ);
  File tmp = LittleFS.open(LOG_TMP_PATH, FILE_WRITE);
  if (!src || !tmp) { if (src) src.close(); if (tmp) tmp.close(); return; }
  String header = src.readStringUntil('\n');
  tmp.println(header.length() ? header : String(LOG_HEADER));
  int32_t seen = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (!line.length()) continue;
    if (seen++ >= skipRows) tmp.println(line);
  }
  src.close(); tmp.close();
  LittleFS.remove(LOG_PATH);
  LittleFS.rename(LOG_TMP_PATH, LOG_PATH);
}

void appendSessionRecord(const SessionRecord &r) {
  if (!littleFsReady) {
    Serial.println("[storage] LittleFS unavailable; record not written to flash.");
    return;
  }
  ensureLogFile();
  File f = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!f) { Serial.println("[storage] failed to open history file."); return; }
  f.print(r.sequence); f.print(',');
  f.print(r.timestampUtc); f.print(',');
  f.print(r.uptimeMs); f.print(',');
  f.print(DEVICE_ID); f.print(',');
  if (isfinite(r.platformKg)) { f.print(r.platformKg, 3); } f.print(',');
  if (isfinite(r.grabBarKgEq)) { f.print(r.grabBarKgEq, 3); } f.print(',');
  if (isfinite(r.trueWeightKg)) { f.print(r.trueWeightKg, 3); } f.print(',');
  f.print(r.weightValid ? 1 : 0); f.print(',');
  if (isfinite(r.heartRateBpm)) { f.print(r.heartRateBpm, 2); } f.print(',');
  f.print(r.heartRateValid ? 1 : 0); f.print(',');
  if (isfinite(r.heartRateCv)) { f.print(r.heartRateCv, 3); } f.print(',');
  if (isfinite(r.impedanceAdcMean)) { f.print(r.impedanceAdcMean, 2); } f.print(',');
  if (isfinite(r.impedanceAdcStd)) { f.print(r.impedanceAdcStd, 2); } f.print(',');
  f.print(','); // impedance in ohms deliberately blank until front end is calibrated
  f.print(r.impedancePathVerified ? "ADC_RAW_FRONT_END_VERIFIED" : "ADC_RAW_UNVERIFIED"); f.print(',');
  f.print(r.state); f.print(',');
  f.print(jsonSafeCsv(r.reason)); f.print(',');
  f.print(r.floorCount); f.print(',');
  f.print(r.barCount); f.print(',');
  f.print(r.adcCount); f.print(',');
  f.print(r.weightThreshold ? 1 : 0); f.print(',');
  f.println(r.heartThreshold ? 1 : 0);
  f.flush(); f.close();
  rotateLogIfNeeded();
}

void printLogTail(uint8_t count = 10) {
  if (!littleFsReady || !LittleFS.exists(LOG_PATH)) { Serial.println("No local history file yet."); return; }
  String tail[10];
  if (count > 10) count = 10;
  uint8_t index = 0;
  File f = LittleFS.open(LOG_PATH, FILE_READ);
  if (!f) { Serial.println("Unable to read history file."); return; }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (!line.length()) continue;
    tail[index] = line;
    index = (index + 1) % count;
  }
  f.close();
  Serial.println("--- latest history rows ---");
  for (uint8_t i = 0; i < count; ++i) {
    String &line = tail[(index + i) % count];
    if (line.length()) Serial.println(line);
  }
  Serial.println("--- end ---");
}

// --------------------------- MODEM COMMANDS ------------------------------
void clearGsmInput() { while (GSMSerial.available()) GSMSerial.read(); }

String readGsmUntil(const char *tokenA, const char *tokenB, uint32_t timeoutMs) {
  String response;
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    while (GSMSerial.available()) {
      char c = (char)GSMSerial.read();
      response += c;
      if ((tokenA && response.indexOf(tokenA) >= 0) || (tokenB && response.indexOf(tokenB) >= 0)) return response;
      if (response.length() > 1800) response.remove(0, 600);
    }
    delay(1);
    yield();
  }
  return response;
}

String sendAT(const String &command, uint32_t timeoutMs = 2000) {
  clearGsmInput();
  GSMSerial.print(command);
  GSMSerial.print("\r");
  return readGsmUntil("OK", "ERROR", timeoutMs);
}

bool modemRegistered() {
  String r = sendAT("AT+CREG?", 2500);
  bool registered = r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0;
  if (!registered) {
    String csq = sendAT("AT+CSQ", 1800);
    Serial.printf("[gsm] registration not confirmed. %s\n", r.c_str());
    Serial.printf("[gsm] signal query: %s\n", csq.c_str());
  }
  return registered;
}

bool setClockFromGsmResponse(const String &response) {
  int p = response.indexOf("+CCLK:");
  if (p < 0) return false;
  int q1 = response.indexOf('"', p);
  int q2 = (q1 >= 0) ? response.indexOf('"', q1 + 1) : -1;
  if (q1 < 0 || q2 < 0) return false;
  String ts = response.substring(q1 + 1, q2);
  int yy, mo, dd, hh, mm, ss, tzq = 0;
  char sign = '+';
  int parsed = sscanf(ts.c_str(), "%2d/%2d/%2d,%2d:%2d:%2d%c%2d", &yy, &mo, &dd, &hh, &mm, &ss, &sign, &tzq);
  if (parsed < 6 || yy < 24 || mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mm > 59 || ss > 60) return false;
  int year = 2000 + yy;
  int64_t seconds = daysFromCivil(year, (unsigned)mo, (unsigned)dd) * 86400LL + hh * 3600LL + mm * 60LL + ss;
  if (parsed >= 8) {
    int offset = tzq * 15 * 60;
    if (sign == '+') seconds -= offset; else if (sign == '-') seconds += offset;
  }
  struct timeval tv;
  tv.tv_sec = (time_t)seconds;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  clockValid = true;
  Serial.printf("[time] clock synced from modem: %s\n", utcTimestamp().c_str());
  return true;
}

bool refreshClockFromModem() {
  if (!gsmResponding) return false;
  String r = sendAT("AT+CCLK?", 2000);
  bool ok = setClockFromGsmResponse(r);
  if (!ok) Serial.println("[time] modem clock unavailable; timestamps remain UNSET until time is available.");
  lastClockRefreshMs = millis();
  return ok;
}

bool initModem() {
  GSMSerial.begin(9600, SERIAL_8N1, PIN_SIM800_RX, PIN_SIM800_TX);
  delay(300);
  bool atOK = false;
  for (int i = 0; i < 3; ++i) {
    String r = sendAT("AT", 1500);
    if (r.indexOf("OK") >= 0) { atOK = true; break; }
    delay(300);
  }
  if (!atOK) {
    gsmResponding = false;
    Serial.println("[gsm] no AT response. SMS remains queued; check power, UART wiring, SIM and antenna.");
    return false;
  }
  sendAT("ATE0", 1200);
  sendAT("AT+CMGF=1", 1800);
  sendAT("AT+CSCS=\"GSM\"", 1800);
  sendAT("AT+CLTS=1", 1800); // network time, if supported
  sendAT("AT&W", 1800);
  gsmResponding = true;
  Serial.println("[gsm] modem responds to AT commands.");
  refreshClockFromModem();
  return true;
}

bool validPhoneNumber(const String &number) {
  if (number.length() < 10 || number.length() > 16) return false;
  int digits = 0;
  for (size_t i = 0; i < number.length(); ++i) {
    char c = number[i];
    if (c >= '0' && c <= '9') digits++;
    else if (!(c == '+' && i == 0)) return false;
  }
  return digits >= 10;
}

bool sendSmsNow(const String &message) {
  if (!validPhoneNumber(familyPhone)) {
    Serial.println("[gsm] no valid family phone saved. Use: phone +91XXXXXXXXXX");
    return false;
  }
  if (!gsmResponding) {
    if (!initModem()) return false;
  }
  if (!modemRegistered()) return false;
  String mode = sendAT("AT+CMGF=1", 2000);
  if (mode.indexOf("OK") < 0) { Serial.println("[gsm] cannot enter SMS text mode."); return false; }

  clearGsmInput();
  GSMSerial.print("AT+CMGS=\"");
  GSMSerial.print(familyPhone);
  GSMSerial.print("\"\r");
  String prompt = readGsmUntil(">", "ERROR", 6000);
  if (prompt.indexOf('>') < 0) {
    Serial.printf("[gsm] SMS prompt missing: %s\n", prompt.c_str());
    return false;
  }
  GSMSerial.print(message);
  GSMSerial.write((uint8_t)26); // Ctrl-Z submits the SMS
  String result = readGsmUntil("+CMGS:", "ERROR", 20000);
  if (result.indexOf("+CMGS:") >= 0) {
    // Drain any trailing OK without needing it for success confirmation.
    readGsmUntil("OK", "ERROR", 1200);
    Serial.println("[gsm] SMS accepted by modem.");
    return true;
  }
  Serial.printf("[gsm] SMS submission failed or timed out: %s\n", result.c_str());
  return false;
}

void queueSms(uint8_t state, const String &message) {
  pendingSmsState = state;
  pendingSmsText = message;
  smsPending = true;
  savePendingSms(); // queue survives a reset
  Serial.println("[gsm] SMS queued for submission.");
}

void serviceSmsQueue() {
  if (!smsPending || captureActive) return;
  if (!validPhoneNumber(familyPhone)) return; // keep queued until a recipient is configured
  if (lastSmsTryMs != 0 && millis() - lastSmsTryMs < GSM_RETRY_MS) return;
  lastSmsTryMs = millis();
  if (sendSmsNow(pendingSmsText)) {
    if (pendingSmsState == STATE_WATCH || pendingSmsState == STATE_ELEVATED_PATTERN) {
      baseline.lastNotifiedState = pendingSmsState;
      saveBaseline();
    }
    smsPending = false;
    pendingSmsText = "";
    pendingSmsState = STATE_NONE;
    savePendingSms();
  } else {
    Serial.println("[gsm] queued SMS retained; another attempt will occur later.");
  }
}

// ---------------------------- AUDIO --------------------------------------
void initAudio() {
  DFSerial.begin(9600, SERIAL_8N1, PIN_DFPLAYER_RX, PIN_DFPLAYER_TX);
  delay(250);
  audioReady = player.begin(DFSerial, false, true);
  if (audioReady) {
    player.volume(18); // range 0..30; tune on the actual speaker
    Serial.println("[audio] DFPlayer ready. Expected files: /mp3/0001.mp3 through /mp3/0005.mp3");
  } else {
    Serial.println("[audio] DFPlayer not detected; firmware will continue without audio.");
  }
}

void playTrack(uint8_t track) {
  if (!audioReady) { Serial.println("[audio] unavailable; no track played."); return; }
  if (track < 1 || track > 5) return;
  player.playMp3Folder(track);
}

uint8_t trackForState(const String &state) {
  if (state == "NO_RULE_TRIGGER") return 1;       // “Measurement complete; no configured warning change detected.”
  if (state == "WATCH") return 2;                 // gentle repeat/check message
  if (state == "ELEVATED_PATTERN") return 3;      // prompt family/professional follow-up
  if (state == "CALIBRATING") return 5;            // setup/baseline not ready
  return 4;                                         // repeat/poor-quality/insufficient data
}

// -------------------------- BASELINE / RULES -----------------------------
float baselineMedian(const float *v, uint8_t n) {
  if (n == 0) return NAN;
  float tmp[BASELINE_TARGET];
  for (uint8_t i = 0; i < n; ++i) tmp[i] = v[i];
  return medianOf(tmp, n);
}

void addBaselineValue(bool isWeight, float value) {
  if (!isfinite(value)) return;
  if (isWeight) {
    if (baseline.weightN < BASELINE_TARGET) baseline.weights[baseline.weightN++] = value;
  } else {
    if (baseline.heartN < BASELINE_TARGET) baseline.heartRates[baseline.heartN++] = value;
  }
}

bool exceedsWeightThreshold(float value) {
  if (baseline.weightN < BASELINE_TARGET) return false;
  float b = baselineMedian(baseline.weights, baseline.weightN);
  if (!isfinite(b) || b <= 1.0f) return false;
  return value >= b * (1.0f + WEIGHT_RISE_THRESHOLD_PCT / 100.0f);
}

bool exceedsHeartThreshold(float value) {
  if (baseline.heartN < BASELINE_TARGET) return false;
  float b = baselineMedian(baseline.heartRates, baseline.heartN);
  if (!isfinite(b) || b <= 1.0f) return false;
  return value >= b * (1.0f + HR_RISE_THRESHOLD_PCT / 100.0f);
}

void pushTrendFlag(bool weightChannel, bool exceeded) {
  if (weightChannel) {
    baseline.weightFlags = ((baseline.weightFlags << 1) | (exceeded ? 1 : 0)) & 0x07;
    if (baseline.weightTrendN < 3) baseline.weightTrendN++;
  } else {
    baseline.heartFlags = ((baseline.heartFlags << 1) | (exceeded ? 1 : 0)) & 0x07;
    if (baseline.heartTrendN < 3) baseline.heartTrendN++;
  }
}

String assessSession(SessionRecord &r) {
  bool wBaseReady = baseline.weightN >= BASELINE_TARGET;
  bool hBaseReady = baseline.heartN >= BASELINE_TARGET;
  bool evaluatedAny = false;
  bool addWeightBaseline = false;
  bool addHeartBaseline = false;

  if (r.weightValid) {
    if (wBaseReady) {
      r.weightThreshold = exceedsWeightThreshold(r.trueWeightKg);
      pushTrendFlag(true, r.weightThreshold);
      evaluatedAny = true;
    } else {
      addWeightBaseline = true;
    }
  }
  if (r.heartRateValid) {
    if (hBaseReady) {
      r.heartThreshold = exceedsHeartThreshold(r.heartRateBpm);
      pushTrendFlag(false, r.heartThreshold);
      evaluatedAny = true;
    } else {
      addHeartBaseline = true;
    }
  }

  // Baseline samples should represent separate daily sessions, not repeated bench captures.
  // Prefer network time from SIM800L. Without a real clock, accept at most one baseline
  // session per boot to reduce accidental over-calibration during rapid repeated captures.
  bool baselineIntervalEligible = false;
  if (clockValid) {
    int64_t nowEpoch = (int64_t)time(nullptr);
    baselineIntervalEligible = (baseline.lastSessionEpoch == 0) ||
                               (nowEpoch - baseline.lastSessionEpoch >= 20LL * 60LL * 60LL);
  } else {
    // No trusted clock? Use an uptime cooldown while the device stays powered.
    // A dedicated RTC is needed to enforce the same interval across long power losses.
    baselineIntervalEligible = (lastBaselineMs == 0) ||
                               (millis() - lastBaselineMs >= AUTO_COOLDOWN_MS);
  }
  if (baselineIntervalEligible) {
    if (addWeightBaseline) addBaselineValue(true, r.trueWeightKg);
    if (addHeartBaseline) addBaselineValue(false, r.heartRateBpm);
    if (addWeightBaseline || addHeartBaseline) lastBaselineMs = millis();
  }

  bool weightPersistent = baseline.weightTrendN >= 3 && bitCount3(baseline.weightFlags) >= 2;
  bool heartPersistent = baseline.heartTrendN >= 3 && bitCount3(baseline.heartFlags) >= 2;

  if (weightPersistent && heartPersistent) {
    r.state = "ELEVATED_PATTERN";
    r.reason = "Persistent upward changes in both weight and resting heart rate compared with this person's baseline.";
  } else if (weightPersistent) {
    r.state = "WATCH";
    r.reason = "Weight has been above the configured personal-baseline threshold in at least 2 of the last 3 valid sessions.";
  } else if (heartPersistent) {
    r.state = "WATCH";
    r.reason = "Resting heart rate has been above the configured personal-baseline threshold in at least 2 of the last 3 valid sessions.";
  } else if (baseline.weightN < BASELINE_TARGET || baseline.heartN < BASELINE_TARGET) {
    r.state = "CALIBRATING";
    r.reason = "Collecting personal baseline readings. No health conclusion can be made yet.";
  } else if (!evaluatedAny) {
    r.state = "INSUFFICIENT_DATA";
    r.reason = "The current session did not provide enough valid measurements for assessment.";
  } else if (!r.weightValid || !r.heartRateValid || !r.impedancePathVerified) {
    r.state = "INSUFFICIENT_DATA";
    r.reason = "At least one measurement channel is unavailable, unreliable, or not yet calibrated. Impedance is currently raw ADC only; this is not an all-clear result.";
  } else {
    r.state = "NO_RULE_TRIGGER";
    r.reason = "No configured trend rule triggered in the valid readings. This does not rule out a health problem.";
  }

  // Impedance is explicitly not scored yet because the uploaded analogue design does not
  // establish a validated excitation/demodulation-to-ohms transfer function.
  baseline.sessions++;
  if (clockValid) baseline.lastSessionEpoch = (int64_t)time(nullptr);
  saveBaseline();
  return r.state;
}

String messageForState(const SessionRecord &r) {
  // Keep the alert compact enough for a single ordinary GSM SMS. Detailed reasons remain in the CSV log.
  if (r.state == "WATCH") {
    if (r.reason.startsWith("Weight")) {
      return String("SteadyStep ") + DEVICE_ID + " WATCH: repeated weight rise >=2% from baseline. Please check on the person. Monitoring only, not diagnosis.";
    }
    return String("SteadyStep ") + DEVICE_ID + " WATCH: resting heart rate repeatedly >=15% above baseline. Please check on the person. Not a diagnosis.";
  }
  if (r.state == "ELEVATED_PATTERN") {
    return String("SteadyStep ") + DEVICE_ID + " ALERT: repeated weight and heart-rate rises vs baseline. Please check and contact their clinician promptly. Monitoring only, not diagnosis.";
  }
  return "";
}

// --------------------------- SIGNAL PROCESSING ---------------------------
float summarizeKgMedian(const int32_t *raw, const uint16_t *times, int count,
                        const LoadCellCalibration &cal, uint16_t skipMs) {
  if (!validCalibration(cal) || count <= 0) return NAN;
  static float work[MAX_FLOOR_SAMPLES];
  int n = 0;
  for (int i = 0; i < count && n < MAX_FLOOR_SAMPLES; ++i) {
    if (times[i] < skipMs) continue; // discard stance/settling transient
    float kg = rawToKg(raw[i], cal);
    if (isfinite(kg) && kg > -50.0f && kg < 300.0f) work[n++] = kg;
  }
  if (n < 20) return NAN;
  return medianOf(work, n);
}

bool estimateHeartRate(float &bpm, float &cvOut) {
  bpm = NAN; cvOut = NAN;
  if (floorN < 100) return false;
  uint16_t firstT = floorTimeBuf[0], lastT = floorTimeBuf[floorN - 1];
  if (lastT <= firstT) return false;
  float fs = (float)(floorN - 1) * 1000.0f / (float)(lastT - firstT);
  if (fs < 35.0f) {
    Serial.printf("[bcg] insufficient sample rate: %.1f Hz. HX711 RATE must be set to 80 SPS for this experiment.\n", fs);
    return false;
  }

  static float hp[MAX_FLOOR_SAMPLES];
  float dc = (float)floorRawBuf[0];
  double sqSum = 0.0;
  int used = 0;
  for (int i = 0; i < floorN; ++i) {
    float x = (float)floorRawBuf[i];
    dc += 0.045f * (x - dc); // remove slow load/body sway; experimental filter
    hp[i] = x - dc;
    if (floorTimeBuf[i] >= 1500) { sqSum += (double)hp[i] * hp[i]; used++; }
  }
  if (used < 100) return false;
  float rms = sqrtf((float)(sqSum / used));
  if (!isfinite(rms) || rms < 1.0f) return false;
  float threshold = fmaxf(0.70f * rms, 1.0f);

  uint16_t peakTimes[80];
  uint8_t peakCount = 0;
  uint16_t lastPeak = 0;
  for (int i = 2; i < floorN - 2; ++i) {
    if (floorTimeBuf[i] < 1500) continue;
    bool localPeak = hp[i] > threshold && hp[i] >= hp[i - 1] && hp[i] > hp[i + 1];
    if (!localPeak) continue;
    uint16_t t = floorTimeBuf[i];
    if (peakCount > 0 && (uint16_t)(t - lastPeak) < 300) continue; // refractory interval
    if (peakCount < 80) peakTimes[peakCount++] = t;
    lastPeak = t;
  }
  if (peakCount < 5) return false;

  float intervals[79];
  int nIntervals = 0;
  for (uint8_t i = 1; i < peakCount; ++i) {
    float dt = (float)(peakTimes[i] - peakTimes[i - 1]) / 1000.0f;
    if (dt >= 0.30f && dt <= 1.50f) intervals[nIntervals++] = dt;
  }
  if (nIntervals < 3) return false;
  float mean = 0.0f;
  for (int i = 0; i < nIntervals; ++i) mean += intervals[i];
  mean /= nIntervals;
  float variance = 0.0f;
  for (int i = 0; i < nIntervals; ++i) variance += (intervals[i] - mean) * (intervals[i] - mean);
  variance /= nIntervals;
  float cv = sqrtf(variance) / mean;
  float rate = 60.0f / mean;
  if (rate < 40.0f || rate > 180.0f || cv > 0.35f) return false;
  bpm = rate;
  cvOut = cv;
  return true;
}

float computeMean(const uint16_t *values, int count) {
  if (count <= 0) return NAN;
  double sum = 0.0;
  for (int i = 0; i < count; ++i) sum += values[i];
  return (float)(sum / count);
}

float computeStd(const uint16_t *values, int count, float mean) {
  if (count < 2 || !isfinite(mean)) return NAN;
  double sum = 0.0;
  for (int i = 0; i < count; ++i) { double d = values[i] - mean; sum += d * d; }
  return sqrtf((float)(sum / (count - 1)));
}

// ------------------------------ SESSIONS ---------------------------------
void resetCaptureBuffers() {
  floorN = barN = adcN = 0;
  totalSamplesDropped = 0;
}

void startCapture(bool manual) {
  if (captureActive) { Serial.println("[session] already capturing."); return; }
  resetCaptureBuffers();
  captureActive = true;
  manualCapture = manual;
  captureStartMs = millis();
  lastAdcSampleMs = 0;
  presenceTiming = false;
  Serial.println(manual ? "[session] manual 10-second capture started." : "[session] person detected; 10-second capture started.");
  Serial.println("[session] stand as still as safely possible and hold the bar normally.");
}

void sampleDuringCapture() {
  uint32_t elapsed = millis() - captureStartMs;
  if (floorHx.is_ready()) {
    long raw = floorHx.read();
    lastFloorRaw = raw; lastFloorReadMs = millis();
    if (floorN < MAX_FLOOR_SAMPLES) {
      floorRawBuf[floorN] = (int32_t)raw;
      floorTimeBuf[floorN] = (uint16_t)((elapsed > 65535U) ? 65535U : elapsed);
      floorN++;
    } else totalSamplesDropped++;
  }
  if (barHx.is_ready()) {
    long raw = barHx.read();
    lastBarRaw = raw; lastBarReadMs = millis();
    if (barN < MAX_BAR_SAMPLES) {
      barRawBuf[barN] = (int32_t)raw;
      barTimeBuf[barN] = (uint16_t)((elapsed > 65535U) ? 65535U : elapsed);
      barN++;
    } else totalSamplesDropped++;
  }
  if (millis() - lastAdcSampleMs >= IMPEDANCE_SAMPLE_PERIOD_MS) {
    lastAdcSampleMs = millis();
    uint16_t adc = (uint16_t)analogRead(PIN_IMPEDANCE_ADC);
    if (adcN < MAX_ADC_SAMPLES) {
      adcBuf[adcN] = adc;
      adcTimeBuf[adcN] = (uint16_t)((elapsed > 65535U) ? 65535U : elapsed);
      adcN++;
    } else totalSamplesDropped++;
  }
  if (elapsed >= SESSION_LENGTH_MS) finishCapture();
}

void queueAlertIfNeeded(const SessionRecord &r) {
  if (r.state == "NO_RULE_TRIGGER") {
    // A valid clear session resets the notification state for a future new event.
    baseline.lastNotifiedState = STATE_NONE;
    saveBaseline();
    return;
  }
  if (r.state != "WATCH" && r.state != "ELEVATED_PATTERN") return;
  uint8_t current = (r.state == "WATCH") ? STATE_WATCH : STATE_ELEVATED_PATTERN;
  bool shouldNotify = (baseline.lastNotifiedState == STATE_NONE) ||
                      (current == STATE_ELEVATED_PATTERN && baseline.lastNotifiedState == STATE_WATCH);
  if (!shouldNotify) return;
  if (smsPending && (pendingSmsState == current || pendingSmsState == STATE_ELEVATED_PATTERN)) return;
  String msg = messageForState(r);
  queueSms(current, msg);
}

void finishCapture() {
  captureActive = false;
  SessionRecord r;
  r.sequence = baseline.sessions + 1;
  r.uptimeMs = millis();
  r.timestampUtc = utcTimestamp();
  r.floorCount = floorN;
  r.barCount = barN;
  r.adcCount = adcN;

  // Use medians after the first 2 seconds to reduce step-on transients.
  r.platformKg = summarizeKgMedian(floorRawBuf, floorTimeBuf, floorN, floorCal, 2000);
  r.grabBarKgEq = summarizeKgMedian(barRawBuf, barTimeBuf, barN, barCal, 2000);
  r.weightValid = validCalibration(floorCal) && validCalibration(barCal) &&
                  isfinite(r.platformKg) && isfinite(r.grabBarKgEq) &&
                  r.platformKg >= 0.0f && r.platformKg < 300.0f &&
                  r.grabBarKgEq > -30.0f && r.grabBarKgEq < 100.0f && floorN >= 20 && barN >= 20;
  if (r.weightValid) {
    r.trueWeightKg = r.platformKg + r.grabBarKgEq;
    if (r.trueWeightKg < 20.0f || r.trueWeightKg > 250.0f) r.weightValid = false;
  }

  r.heartRateValid = estimateHeartRate(r.heartRateBpm, r.heartRateCv);
  if (adcN > 0) {
    r.impedanceAdcMean = computeMean(adcBuf, adcN);
    r.impedanceAdcStd = computeStd(adcBuf, adcN, r.impedanceAdcMean);
  }

  assessSession(r);
  appendSessionRecord(r);
  printSessionSummary(r);
  playTrack(trackForState(r.state));
  queueAlertIfNeeded(r);

  // Enforce a measurement-session cooldown after either an automatic or manual capture.
  autoCaptureDoneThisBoot = true;
  nextAutoCaptureMs = millis() + AUTO_COOLDOWN_MS;
  if (clockValid) baseline.lastSessionEpoch = (int64_t)time(nullptr);
  saveBaseline();
  Serial.printf("[session] finished. Samples floor=%u bar=%u ADC=%u dropped=%lu\n", floorN, barN, adcN, (unsigned long)totalSamplesDropped);
}

void printSessionSummary(const SessionRecord &r) {
  Serial.println("\n========== STEADYSTEP SESSION ==========");
  Serial.printf("Session: %lu | time: %s\n", (unsigned long)r.sequence, r.timestampUtc.c_str());
  Serial.printf("Platform force: "); if (isfinite(r.platformKg)) Serial.printf("%.2f kg-equivalent\n", r.platformKg); else Serial.println("INVALID / NOT CALIBRATED");
  Serial.printf("Grab-bar vertical force: "); if (isfinite(r.grabBarKgEq)) Serial.printf("%.2f kg-equivalent\n", r.grabBarKgEq); else Serial.println("INVALID / NOT CALIBRATED");
  Serial.printf("True-weight estimate: "); if (r.weightValid) Serial.printf("%.2f kg\n", r.trueWeightKg); else Serial.println("INVALID — calibrate BOTH channels and verify bar-force direction");
  Serial.printf("BCG heart-rate estimate: "); if (r.heartRateValid) Serial.printf("%.1f bpm (interval CV %.2f)\n", r.heartRateBpm, r.heartRateCv); else Serial.println("UNAVAILABLE / LOW QUALITY — experimental, not validated");
  Serial.printf("Foot-electrode ADC: mean=%.1f counts, SD=%.1f; impedance in ohms NOT computed\n", r.impedanceAdcMean, r.impedanceAdcStd);
  Serial.printf("Status: %s\nReason: %s\n", r.state.c_str(), r.reason.c_str());
  Serial.printf("Baseline samples: weight %u/%u, heart %u/%u\n", baseline.weightN, BASELINE_TARGET, baseline.heartN, BASELINE_TARGET);
  Serial.println("========================================\n");
}

void serviceAutomaticPresenceDetection() {
  if (captureActive) return;
  if (autoCaptureDoneThisBoot) {
    if (nextAutoCaptureMs == 0 || (int32_t)(millis() - nextAutoCaptureMs) < 0) return;
    autoCaptureDoneThisBoot = false; // allow a new daily session after the cooldown
  }
  if (nextAutoCaptureMs != 0 && (int32_t)(millis() - nextAutoCaptureMs) < 0) return;
  if (!validCalibration(floorCal) || !validCalibration(barCal)) return;
  if (clockValid && baseline.lastSessionEpoch > 0) {
    int64_t sinceLast = (int64_t)time(nullptr) - baseline.lastSessionEpoch;
    if (sinceLast >= 0 && sinceLast < 20LL * 60LL * 60LL) return;
  }

  if (floorHx.is_ready()) { lastFloorRaw = floorHx.read(); lastFloorReadMs = millis(); }
  if (barHx.is_ready()) { lastBarRaw = barHx.read(); lastBarReadMs = millis(); }
  if (millis() - lastFloorReadMs > 500 || millis() - lastBarReadMs > 500) { presenceTiming = false; return; }
  float platformKg = rawToKg(lastFloorRaw, floorCal);
  float barKg = rawToKg(lastBarRaw, barCal);
  float combined = platformKg + barKg;
  if (isfinite(combined) && combined >= PRESENCE_FORCE_KG_EQ) {
    if (!presenceTiming) { presenceTiming = true; presenceStartMs = millis(); }
    if (millis() - presenceStartMs >= AUTO_PRESENCE_STABLE_MS) startCapture(false);
  } else {
    presenceTiming = false;
  }
}

// ---------------------------- CALIBRATION -------------------------------
bool tareChannel(HX711 &hx, LoadCellCalibration &cal, const char *name, const char *prefix) {
  Serial.printf("[cal] tare %s: ensure the sensor is unloaded, then wait...\n", name);
  if (!hx.wait_ready_timeout(2000)) { Serial.println("[cal] HX711 not ready."); return false; }
  long raw = hx.read_average(10);
  cal.offset = (int32_t)raw;
  cal.countsPerKg = 0.0f; // force the known-weight calibration to be repeated
  saveCalibration(prefix, cal);
  Serial.printf("[cal] %s offset stored: %ld. Now apply a known load and use: cal %s <kg>\n", name, raw, name);
  return true;
}

bool calibrateChannel(HX711 &hx, LoadCellCalibration &cal, float knownKg, const char *name, const char *prefix) {
  if (!isfinite(knownKg) || knownKg < 0.5f || knownKg > 200.0f) {
    Serial.println("[cal] known load must be between 0.5 and 200 kg-equivalent."); return false;
  }
  if (!hx.wait_ready_timeout(2000)) { Serial.println("[cal] HX711 not ready."); return false; }
  long raw = hx.read_average(10);
  long delta = raw - cal.offset;
  if (labs(delta) < 1000) { Serial.println("[cal] raw change too small. Confirm load, wiring and tare first."); return false; }
  cal.countsPerKg = (float)delta / knownKg; // signed: load-cell orientation can invert sign
  saveCalibration(prefix, cal);
  Serial.printf("[cal] %s calibrated: offset=%ld counts/kg=%.3f\n", name, (long)cal.offset, cal.countsPerKg);
  return true;
}

void resetBaseline() {
  memset(&baseline, 0, sizeof(baseline));
  baseline.version = 1;
  baseline.lastNotifiedState = STATE_NONE;
  saveBaseline();
  Serial.println("[baseline] cleared. The next valid sessions will establish a new baseline.");
}

void printStatus() {
  Serial.println("\n--- SteadyStep status ---");
  Serial.printf("Device: %s\n", DEVICE_ID);
  Serial.printf("Platform HX711: %s | calibration: %s\n", floorHx.is_ready() ? "READY" : "NOT READY", validCalibration(floorCal) ? "SET" : "NOT SET");
  Serial.printf("Grab-bar HX711: %s | calibration: %s\n", barHx.is_ready() ? "READY" : "NOT READY", validCalibration(barCal) ? "SET" : "NOT SET");
  Serial.printf("BCG: experimental load-cell pathway; requires HX711 RATE=80 SPS and validation\n");
  Serial.printf("Impedance: %s (raw ADC only; no ohm conversion / no alert scoring)\n", IMPEDANCE_FRONT_END_VERIFIED ? "front-end marked verified" : "front-end unverified");
  Serial.printf("DFPlayer: %s\n", audioReady ? "READY" : "NOT DETECTED");
  Serial.printf("SIM800L: %s | phone: %s | SMS queue: %s\n", gsmResponding ? "RESPONDING" : "NOT RESPONDING", validPhoneNumber(familyPhone) ? familyPhone.c_str() : "NOT CONFIGURED", smsPending ? "PENDING" : "empty");
  Serial.printf("Clock: %s\n", utcTimestamp().c_str());
  Serial.printf("LittleFS: %s", littleFsReady ? "READY" : "NOT READY");
  if (littleFsReady) Serial.printf(" | used %lu / %lu bytes", (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
  Serial.println();
  Serial.printf("Baseline: weight %u/%u, heart %u/%u | session records=%lu\n", baseline.weightN, BASELINE_TARGET, baseline.heartN, BASELINE_TARGET, (unsigned long)baseline.sessions);
  Serial.println("Dashboard integration: intentionally not implemented yet.");
  Serial.println("-------------------------\n");
}

void printHelp() {
  Serial.println("\nSteadyStep commands:");
  Serial.println("  help                         show commands");
  Serial.println("  status                       show hardware/storage/network status");
  Serial.println("  tare floor                   platform channel unloaded; sets offset and clears scale factor");
  Serial.println("  tare bar                     grab-bar channel unloaded; sets offset and clears scale factor");
  Serial.println("  cal floor <kg>               apply known kg to platform only after tare");
  Serial.println("  cal bar <kg>                 apply known vertical force to bar sensor in a safe test fixture");
  Serial.println("  capture                      force one 10-second test capture");
  Serial.println("  baseline_reset               clear personal baseline samples");
  Serial.println("  phone +91XXXXXXXXXX          save family phone number");
  Serial.println("  test_audio normal|watch|danger|retry|calibrating");
  Serial.println("  test_sms                     queue a test SMS (only to configured number)");
  Serial.println("  log_tail                     print recent measurement history rows");
  Serial.println("  storage                      show local flash usage");
  Serial.println("  format_fs                    show warning; type format_fs_confirm to erase and initialize filesystem");
  Serial.println("\nDefault thresholds are illustrative engineering settings, NOT validated clinical thresholds.");
  Serial.println("Impedance ohms and impedance-based alerts are disabled until circuit/calibration are verified.\n");
}

void processSerialCommand(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  String lower = cmd; lower.toLowerCase();

  if (lower == "help") { printHelp(); return; }
  if (lower == "status") { printStatus(); return; }
  if (lower == "capture") { startCapture(true); return; }
  if (lower == "baseline_reset") { resetBaseline(); return; }
  if (lower == "log_tail") { printLogTail(); return; }
  if (lower == "storage") {
    if (littleFsReady) Serial.printf("LittleFS used=%lu total=%lu bytes\n", (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
    else Serial.println("LittleFS unavailable.");
    return;
  }
  if (lower == "format_fs") {
    Serial.println("WARNING: this erases all local session history. Type exactly: FORMAT_FS_CONFIRM to proceed.");
    return;
  }
  if (lower == "format_fs_confirm") {
    Serial.println("Formatting LittleFS; all local session history will be erased...");
    if (LittleFS.format() && LittleFS.begin(false)) {
      littleFsReady = true;
      ensureLogFile();
      Serial.println("LittleFS formatted and mounted.");
    } else {
      littleFsReady = false;
      Serial.println("LittleFS format/mount failed. Check board partition scheme.");
    }
    return;
  }

  if (lower.startsWith("phone ")) {
    String number = cmd.substring(6); number.trim();
    if (!validPhoneNumber(number)) { Serial.println("Invalid number. Use +countrycode followed by digits, e.g. phone +919876543210"); return; }
    familyPhone = number;
    prefs.putString("phone", familyPhone);
    lastSmsTryMs = 0;
    Serial.println("Family phone saved. It is stored in this device's NVS.");
    return;
  }

  if (lower == "test_sms") {
    if (!validPhoneNumber(familyPhone)) { Serial.println("Configure a family phone first using: phone +91XXXXXXXXXX"); return; }
    queueSms(STATE_NONE, String("SteadyStep test message from device ") + DEVICE_ID + ". This is a connectivity test, not a medical alert.");
    lastSmsTryMs = 0;
    return;
  }

  if (lower.startsWith("test_audio ")) {
    String arg = lower.substring(11); arg.trim();
    if (arg == "normal") playTrack(1);
    else if (arg == "watch") playTrack(2);
    else if (arg == "danger") playTrack(3);
    else if (arg == "retry") playTrack(4);
    else if (arg == "calibrating") playTrack(5);
    else Serial.println("Use: test_audio normal|watch|danger|retry|calibrating");
    return;
  }

  if (lower == "tare floor") { tareChannel(floorHx, floorCal, "floor", "floor"); return; }
  if (lower == "tare bar") { tareChannel(barHx, barCal, "bar", "bar"); return; }

  if (lower.startsWith("cal floor ")) {
    float kg = cmd.substring(10).toFloat();
    calibrateChannel(floorHx, floorCal, kg, "floor", "floor");
    return;
  }
  if (lower.startsWith("cal bar ")) {
    float kg = cmd.substring(8).toFloat();
    calibrateChannel(barHx, barCal, kg, "bar", "bar");
    return;
  }

  Serial.println("Unknown command. Type help.");
}

void serviceSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) { processSerialCommand(line); line = ""; }
    } else if (line.length() < 160) line += c;
  }
}

// -------------------------------- SETUP ----------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nSteadyStep ESP32 Core Firmware — provisional hardware build");
  Serial.println("Monitoring prototype only; not for medical diagnosis or treatment decisions.");

  prefs.begin("steadystep", false);
  loadCalibration("floor", floorCal);
  loadCalibration("bar", barCal);
  loadBaseline();
  familyPhone = prefs.getString("phone", "");
  loadPendingSms();

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_IMPEDANCE_ADC, ADC_11db);
  pinMode(PIN_IMPEDANCE_ADC, INPUT);

  floorHx.begin(PIN_FLOOR_DOUT, PIN_FLOOR_SCK);
  barHx.begin(PIN_BAR_DOUT, PIN_BAR_SCK);
  floorHx.set_gain(128);
  barHx.set_gain(128);

  littleFsReady = LittleFS.begin(false); // never format automatically; protect existing logs
  if (littleFsReady) {
    ensureLogFile();
    Serial.printf("[storage] LittleFS ready: used=%lu total=%lu bytes\n", (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
  } else {
    Serial.println("[storage] LittleFS mount failed. Select a partition scheme with a LittleFS/SPIFFS partition; no automatic format was done.");
  }

  initAudio();
  initModem();
  lastGsmInitTryMs = millis();

  printHelp();
  printStatus();
}

// -------------------------------- LOOP -----------------------------------
void loop() {
  serviceSerial();

  if (captureActive) {
    sampleDuringCapture();
    return; // avoid slow AT transactions during acquisition
  }

  serviceAutomaticPresenceDetection();
  if (captureActive) return; // an automatic session may have started in the call above
  serviceSmsQueue();

  if (!gsmResponding && millis() - lastGsmInitTryMs >= GSM_INIT_RETRY_MS) {
    lastGsmInitTryMs = millis();
    initModem();
  }
  if (gsmResponding && ((lastClockRefreshMs == 0) ||
      (clockValid && millis() - lastClockRefreshMs >= MODEM_CLOCK_REFRESH_MS) ||
      (!clockValid && millis() - lastClockRefreshMs >= 60000UL))) {
    refreshClockFromModem();
  }

  delay(1);
}

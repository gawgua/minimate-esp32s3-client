#include <Arduino.h>
#include <esp_system.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP_I2S.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <Wire.h>

#include "config.h"
#include "face_frames_128x64.h"

namespace {
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t CHUNK_SAMPLES = 800;  // 50 ms, 1600 bytes of PCM16.
constexpr size_t MIN_SAMPLES = SAMPLE_RATE * 4 / 10;
constexpr size_t RECORDING_SAMPLES = SAMPLE_RATE * 5;
constexpr uint32_t REPLY_TIMEOUT_MS = 120000;

// SSD1306 128x64 on I2C, and a push button from BUTTON_PIN to GND.
constexpr int OLED_SDA = 8;
constexpr int OLED_SCL = 9;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;
constexpr int BUTTON_PIN = 7;
constexpr uint32_t DEBOUNCE_MS = 30;
// How long a reply or error face stays before the idle face returns.
constexpr uint32_t RESULT_FACE_MS = 3000;
// A full I2C frame takes ~25 ms. While recording, draw at half rate so audio reads keep up.
constexpr uint32_t RECORDING_FRAME_MS = 133;

enum class State { Idle, Recording, Waiting };

// IDs from the Minimate classifier's config.json id2label mapping.
enum class IntentId : int8_t {
  None = -1,
  CheckFocusSession = 0,
  FocusSessionControl = 1,
  Greeting = 2,
  OpenQuestion = 3,
  OutOfScope = 4,
  RepeatResponse = 5,
  ScheduleReminder = 6,
};

// Indexed by IntentId.
const char *const INTENT_NAMES[] = {
    "check_focus_session", "focus_session_control", "greeting",          "open_question",
    "out_of_scope",        "repeat_response",       "schedule_reminder",
};

// Face shown after each reply, indexed by IntentId. -1 uses Face::Clarification.
constexpr Face INTENT_FACES[] = {
    Face::Speaking,       // check_focus_session
    Face::Speaking,       // focus_session_control
    Face::Speaking,       // greeting
    Face::Speaking,       // open_question
    Face::Clarification,  // out_of_scope
    Face::Speaking,       // repeat_response
    Face::Speaking,       // schedule_reminder
};
static_assert(sizeof(INTENT_FACES) / sizeof(INTENT_FACES[0]) ==
                  static_cast<int>(IntentId::ScheduleReminder) + 1,
              "Give every intent a face");

State state = State::Idle;
I2SClass microphone;
WebSocketsClient socket;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool displayReady = false;
bool displayDirty = true;
bool connected = false;
bool resetConnection = false;
size_t samplesSent = 0;
size_t returnedAudioBytes = 0;
uint32_t waitingSince = 0;

// Reply or error face shown for RESULT_FACE_MS while idle.
Face resultFace = Face::Idle;
uint32_t resultSince = 0;
// Face on screen. Face::Count means a text screen is showing.
Face shownFace = Face::Count;
uint32_t faceSince = 0;
uint8_t shownFrame = 0;
uint32_t drawnAt = 0;

// INMP441 sends 24-bit samples at the start of a 32-bit I2S slot.
uint32_t slots[CHUNK_SAMPLES];
uint8_t pcm[CHUNK_SAMPLES * 2];

// Log lines go to Serial and to the server's /dashboard, so logs stay visible on external
// power without USB. Lines wait here while the server is unreachable; the oldest drop first.
constexpr size_t LOG_QUEUE_LINES = 24;
constexpr size_t LOG_LINE_CHARS = 160;
char logQueue[LOG_QUEUE_LINES][LOG_LINE_CHARS];
size_t logHead = 0;
size_t logCount = 0;
size_t logsDropped = 0;

void deviceLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void deviceLog(const char *format, ...) {
  char line[LOG_LINE_CHARS];
  const uint32_t now = millis();
  // Uptime stamps keep queued lines in order even when they reach the server late.
  const int prefix = snprintf(line, sizeof(line), "LOG [%lu.%03lus] ",
                              static_cast<unsigned long>(now / 1000),
                              static_cast<unsigned long>(now % 1000));
  va_list args;
  va_start(args, format);
  vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
  va_end(args);
  Serial.println(line + 4);  // Serial shows the line without the LOG prefix.
  if (logCount == LOG_QUEUE_LINES) {
    logHead = (logHead + 1) % LOG_QUEUE_LINES;
    --logCount;
    ++logsDropped;
  }
  memcpy(logQueue[(logHead + logCount) % LOG_QUEUE_LINES], line, sizeof(line));
  ++logCount;
}

void flushLogs() {
  if (!connected) return;
  if (logsDropped) {
    char note[64];
    snprintf(note, sizeof(note), "LOG %u older lines dropped while offline",
             static_cast<unsigned>(logsDropped));
    if (!socket.sendTXT(note)) return;
    logsDropped = 0;
  }
  while (logCount) {
    if (!socket.sendTXT(logQueue[logHead])) return;  // Keep the line and retry next loop.
    logHead = (logHead + 1) % LOG_QUEUE_LINES;
    --logCount;
  }
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_BROWNOUT: return "brownout, check the 5 V supply";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}

void showResult(Face face) {
  resultFace = face;
  resultSince = millis();
}

void renderText(const char *title, const char *detail) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(title);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print(detail);
  display.display();
}

Face currentFace(uint32_t now) {
  if (state == State::Recording) return Face::Listening;
  if (state == State::Waiting) return Face::Processing;
  if (resultFace != Face::Idle && now - resultSince < RESULT_FACE_MS) return resultFace;
  return Face::Idle;
}

void updateDisplay() {
  if (!displayReady) return;
  if (WiFi.status() != WL_CONNECTED || !connected) {
    // Text screens help with setup. Draw them once per change.
    if (displayDirty) {
      WiFi.status() != WL_CONNECTED ? renderText("Wi-Fi...", "Connecting to Wi-Fi")
                                    : renderText("Server...", "Connecting to ASR");
      shownFace = Face::Count;
    }
    displayDirty = false;
    return;
  }
  const uint32_t now = millis();
  const Face face = currentFace(now);
  if (face != shownFace) {
    shownFace = face;
    faceSince = now;
    displayDirty = true;
  }
  const uint8_t frame = (uint64_t(now - faceSince) * FACE_OLED_FPS / 1000) % FACE_OLED_FRAMES;
  if (!displayDirty &&
      (frame == shownFrame || (state == State::Recording && now - drawnAt < RECORDING_FRAME_MS))) {
    return;
  }
  memcpy(display.getBuffer(), face_oled_frames[static_cast<int>(face)][frame],
         sizeof(face_oled_frames[0][0]));
  if (state == State::Recording) {
    // Progress bar in the two rows below the face.
    const int width = static_cast<int>(OLED_WIDTH * samplesSent / RECORDING_SAMPLES);
    display.fillRect(0, FACE_OLED_FACE_ROWS, width, OLED_HEIGHT - FACE_OLED_FACE_ROWS,
                     SSD1306_WHITE);
  }
  display.display();
  shownFrame = frame;
  drawnAt = now;
  displayDirty = false;
}

void resetRecording() {
  if (state == State::Recording) {
    microphone.end();
  }
  state = State::Idle;
  returnedAudioBytes = 0;
  displayDirty = true;
}

void socketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      connected = true;
      displayDirty = true;
      deviceLog("ASR connected. Press the button or enter r to record, s to stop.");
      break;
    case WStype_DISCONNECTED:
      if (connected) {
        deviceLog("ASR disconnected. Any current recording was dropped.");
        if (state != State::Idle) showResult(Face::Error);
      }
      connected = false;
      resetRecording();
      break;
    case WStype_BIN:
      // The board has no playback hardware. Consume each chunk as it arrives.
      if (state == State::Waiting) returnedAudioBytes += length;
      break;
    case WStype_TEXT: {
      if (length == 1 && payload[0] >= '0' &&
          payload[0] <= '0' + static_cast<int>(IntentId::ScheduleReminder)) {
        const auto intent = static_cast<IntentId>(payload[0] - '0');
        deviceLog("Intent %d: %s", static_cast<int>(intent), INTENT_NAMES[static_cast<int>(intent)]);
        showResult(INTENT_FACES[static_cast<int>(intent)]);
      } else if (length == 2 && payload[0] == '-' && payload[1] == '1') {
        deviceLog("No intent (%d)", static_cast<int>(IntentId::None));
        showResult(Face::Clarification);
      } else {
        deviceLog("Unexpected ASR reply.");
        showResult(Face::Error);
      }
      if (state == State::Waiting) {
        if (returnedAudioBytes > 0) {
          deviceLog("Filtered audio received: %u bytes",
                    static_cast<unsigned>(returnedAudioBytes));
        }
        resetRecording();
      } else if (state == State::Recording) {
        // An early response means the server rejected the current recording.
        resetConnection = true;
      }
      break;
    }
    default:
      break;
  }
}

void startRecording() {
  if (!connected) {
    deviceLog("Wait for the ASR connection.");
    return;
  }
  if (state != State::Idle) {
    deviceLog("A recording or response is already in progress.");
    return;
  }
  // Send each step at once, so a reset during startup shows how far it got on the dashboard.
  deviceLog("Starting recording, RSSI %d dBm", WiFi.RSSI());
  flushLogs();
  microphone.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  const auto micSlot = MIC_SLOT == 0 ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;
  if (!microphone.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_MONO, micSlot)) {
    deviceLog("Could not start I2S. Check microphone pins.");
    showResult(Face::Error);
    return;
  }
  deviceLog("Mic started, warming up");
  flushLogs();
  // Discard 300 ms so the microphone can settle after its clock starts.
  for (int chunk = 0; chunk < 6; ++chunk) {
    if (microphone.readBytes(reinterpret_cast<char *>(slots), sizeof(slots)) != sizeof(slots)) {
      microphone.end();
      deviceLog("I2S warm-up failed. Check microphone wiring.");
      showResult(Face::Error);
      return;
    }
  }
  samplesSent = 0;
  state = State::Recording;
  resultFace = Face::Idle;
  deviceLog("Recording for up to 5 seconds. Press again or enter s to stop early.");
  flushLogs();
}

void finishRecording() {
  resetRecording();
  if (!socket.sendTXT("END")) {
    deviceLog("Could not send END.");
    showResult(Face::Error);
    socket.disconnect();
    return;
  }
  waitingSince = millis();
  state = State::Waiting;
  deviceLog("Waiting for transcription...");
}

void stopRecording() {
  if (samplesSent >= MIN_SAMPLES) {
    finishRecording();
  } else {
    deviceLog("Record at least 0.4 seconds before stopping.");
  }
}

void sendAudioChunk() {
  const size_t bytesRead = microphone.readBytes(reinterpret_cast<char *>(slots), sizeof(slots));
  constexpr size_t BYTES_PER_FRAME = sizeof(slots[0]);
  if (!bytesRead || bytesRead % BYTES_PER_FRAME != 0) {
    deviceLog("I2S read failed. Dropping recording.");
    showResult(Face::Error);
    resetRecording();
    socket.disconnect();
    return;
  }
  size_t count = bytesRead / BYTES_PER_FRAME;
  if (count > RECORDING_SAMPLES - samplesSent) {
    count = RECORDING_SAMPLES - samplesSent;
  }
  for (size_t i = 0; i < count; ++i) {
    // Keep the top 16 bits of the selected slot and write them little-endian.
    const uint32_t sample = slots[i];
    pcm[2 * i] = static_cast<uint8_t>(sample >> 16);
    pcm[2 * i + 1] = static_cast<uint8_t>(sample >> 24);
  }
  if (!socket.sendBIN(pcm, count * 2)) {
    deviceLog("Could not send audio. Dropping recording.");
    showResult(Face::Error);
    resetRecording();
    socket.disconnect();
    return;
  }
  samplesSent += count;
  if (samplesSent == RECORDING_SAMPLES) {
    finishRecording();
  }
}

// Returns true once per debounced press. The button pulls BUTTON_PIN to GND.
bool buttonPressed() {
  static int stable = HIGH;
  static int last = HIGH;
  static uint32_t changedAt = 0;
  const int reading = digitalRead(BUTTON_PIN);
  const uint32_t now = millis();
  if (reading != last) {
    last = reading;
    changedAt = now;
  }
  if (reading != stable && now - changedAt >= DEBOUNCE_MS) {
    stable = reading;
    return stable == LOW;
  }
  return false;
}

void onButton() {
  deviceLog("Button pressed");
  flushLogs();
  if (state == State::Idle) {
    startRecording();
  } else if (state == State::Recording) {
    stopRecording();
  } else {
    deviceLog("Still waiting for the reply.");
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!displayReady) {
    deviceLog("OLED not found. Check SDA, SCL, power, and the 0x3C address.");
  }
  updateDisplay();
  microphone.setTimeout(100);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  socket.begin(ASR_HOST, ASR_PORT, "/ws/asr", "");
  socket.onEvent(socketEvent);
  socket.setReconnectInterval(2000);
  socket.enableHeartbeat(15000, 3000, 2);
  deviceLog("Boot, reset reason: %s", resetReason());
  deviceLog("Connecting to Wi-Fi and ASR server...");
}

void loop() {
  static bool wifiWasUp = false;
  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasUp) {
    wifiWasUp = wifiUp;
    displayDirty = true;
    if (wifiUp) {
      deviceLog("Wi-Fi connected, IP %s, RSSI %d dBm", WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
    } else {
      deviceLog("Wi-Fi lost");
    }
  }
  if (!wifiUp) {
    if (connected || state != State::Idle) {
      resetRecording();
      socket.disconnect();
      connected = false;
    }
    updateDisplay();
    delay(20);
    return;
  }

  socket.loop();
  flushLogs();
  if (resetConnection || (state == State::Waiting && millis() - waitingSince >= REPLY_TIMEOUT_MS)) {
    deviceLog("Resetting ASR connection after an early reply or response timeout.");
    if (!resetConnection) showResult(Face::Error);
    resetConnection = false;
    resetRecording();
    socket.disconnect();
  }

  if (buttonPressed()) {
    onButton();
  }
  while (Serial.available()) {
    const int command = Serial.read();
    if (command == 'r') {
      startRecording();
    } else if (command == 's' && state == State::Recording) {
      stopRecording();
    }
  }

  updateDisplay();

  if (state == State::Recording) {
    sendAudioChunk();
  } else {
    delay(2);
  }
}

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <driver/ledc.h>
#include <atomic>
#include <math.h>
#include <string.h>
#include "BluetoothA2DPSink.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "This sketch requires classic ESP32 with Bluetooth Classic A2DP."
#endif
#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "Use Arduino-ESP32 3.x (tested build: 3.3.8)."
#endif

static constexpr uint8_t PIN_PWM = 18;
static constexpr uint8_t PWM_BITS = 10;
static constexpr uint32_t PWM_STEPS = 1u << PWM_BITS;
#ifndef BENCH_NO_POWER_STAGE
#define BENCH_NO_POWER_STAGE 0
#endif
static constexpr float BENCH_MAX_ON_US = 3.0f;
static constexpr float MEASURED_MAX_ON_US = 0.0f;
static constexpr float TRANSFORMER_MAX_ON_US =
    BENCH_NO_POWER_STAGE ? BENCH_MAX_ON_US : MEASURED_MAX_ON_US;
static constexpr float DUTY_HARD_CEILING = 0.30f;
static constexpr uint32_t FC_MIN = 10000;
static constexpr uint32_t FC_MAX = 60000;
static constexpr float AUDIO_LP_HZ = 2000.0f;
static constexpr uint32_t WDT_TIMEOUT_MS = 300;
static constexpr uint32_t UPDATE_HZ = 8000;
static constexpr uint32_t UPDATE_US = 1000000 / UPDATE_HZ;
static constexpr int64_t SOFT_START_US = 1000000;
static constexpr int64_t DATA_TIMEOUT_US = 100000;
static constexpr int64_t CONTROL_DEADLINE_US = 2000;
static constexpr uint32_t RB_SIZE = 8192;
static constexpr uint32_t RB_MASK = RB_SIZE - 1;
static constexpr uint32_t PREFILL = 1024;
static constexpr float TARGET_FILL = 1500.0f;
static_assert((RB_SIZE & RB_MASK) == 0, "Ring size must be a power of two");

// Shared by the Bluetooth callback and control task.
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t rb[RB_SIZE];
static uint32_t rbHead = 0, rbTail = 0, rbGeneration = 0;
static int64_t lastAcceptedDataUs = 0;
static std::atomic<bool> connected(false), playing(false), acceptAudio(true);
static std::atomic<uint32_t> sampleRate(44100), sourceEpoch(0);
static std::atomic<bool> stopRequested(false);
static TaskHandle_t controlTaskHandle = nullptr;
static QueueHandle_t commandQueue = nullptr, statusQueue = nullptr;
static esp_timer_handle_t updateTimer = nullptr;
BluetoothA2DPSink a2dp_sink;

enum Mode { MODE_BT, MODE_TONE, MODE_CONST };
// Only the control task changes settings and PWM.
static Mode mode = MODE_BT;
static bool armed = false, pwmReady = false, primed = false;
static bool watchdogReady = false, controlFault = false;
static uint32_t carrierRequested = 25000, carrierReal = 0;
static uint32_t dutyApplied = 0;
static float maxDuty = 0.05f, bias = 0.25f, gain = 0.25f;
static bool sqrtMap = false;
static int64_t maxDtUs = 0;
static uint32_t deadlineTrips = 0;
static float ramp = 0, phase = 0, resampleFraction = 0;
static const char *lastReason = "startup";

struct Status {
  bool armed, pwmReady, connected, playing, watchdogReady, controlFault;
  Mode mode;
  uint32_t carrier, fill, fs, duty, maxCount;
  float maxDuty, bias, gain, ramp;
  bool sqrtMap;
  int64_t maxDtUs;
  uint32_t trips;
  const char *reason;
};

static float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

static void resetAudio() {
  portENTER_CRITICAL(&audioMux);
  rbHead = rbTail = 0;
  lastAcceptedDataUs = 0;
  ++rbGeneration;
  portEXIT_CRITICAL(&audioMux);
}

static uint32_t audioFill(int64_t *lastUs = nullptr) {
  portENTER_CRITICAL(&audioMux);
  uint32_t n = (rbHead - rbTail) & RB_MASK;
  if (lastUs) *lastUs = lastAcceptedDataUs;
  portEXIT_CRITICAL(&audioMux);
  return n;
}

static void wakeControl() {
  if (controlTaskHandle) xTaskNotifyGive(controlTaskHandle);
}

static void invalidateSource() {
  resetAudio();
  sourceEpoch.fetch_add(1, std::memory_order_release);
  wakeControl();
}

static void onConnection(esp_a2d_connection_state_t state, void *) {
  connected.store(state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  if (state != ESP_A2D_CONNECTION_STATE_CONNECTED) playing.store(false);
  invalidateSource();
}

static void onAudioState(esp_a2d_audio_state_t state, void *) {
  playing.store(state == ESP_A2D_AUDIO_STATE_STARTED);
  invalidateSource();
}

static void onSampleRate(uint16_t rate) {
  sampleRate.store(rate);
  invalidateSource();
}

// A2DP delivers interleaved signed 16-bit stereo PCM.
static void onStreamData(const uint8_t *data, uint32_t len) {
  if (!acceptAudio.load() || !connected.load() || !playing.load()) return;
  uint32_t fs = sampleRate.load();
  if (fs != 44100 && fs != 48000) return;
  static uint32_t filterGeneration = UINT32_MAX;
  static float lp1 = 0, lp2 = 0;
  float alpha = 1.0f - expf(-2.0f * PI * AUDIO_LP_HZ / float(fs));
  const uint32_t frames = len / 4;
  for (uint32_t first = 0; first < frames; first += 64) {
    uint32_t generation;
    portENTER_CRITICAL(&audioMux);
    generation = rbGeneration;
    portEXIT_CRITICAL(&audioMux);
    if (filterGeneration != generation) {
      filterGeneration = generation;
      lp1 = lp2 = 0;
    }
    uint32_t count = min(uint32_t(64), frames - first);
    int16_t filtered[64];
    for (uint32_t j = 0; j < count; ++j) {
      int16_t stereo[2];
      memcpy(stereo, data + 4 * (first + j), sizeof(stereo));
      float mono = (int32_t(stereo[0]) + int32_t(stereo[1])) * 0.5f;
      lp1 += alpha * (mono - lp1);
      lp2 += alpha * (lp1 - lp2);
      filtered[j] = int16_t(clampf(lp2, -32768, 32767));
    }
    bool overflow = false, obsolete = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&audioMux);
    obsolete = generation != rbGeneration;
    if (!obsolete) {
      uint32_t freeSlots = RB_MASK - ((rbHead - rbTail) & RB_MASK);
      if (freeSlots < count) {
        rbHead = rbTail = 0;
        lastAcceptedDataUs = 0;
        ++rbGeneration;
        overflow = true;
      } else {
        for (uint32_t j = 0; j < count; ++j) {
          rb[rbHead] = filtered[j];
          rbHead = (rbHead + 1) & RB_MASK;
        }
        lastAcceptedDataUs = now;
      }
    }
    portEXIT_CRITICAL(&audioMux);
    if (overflow) {
      sourceEpoch.fetch_add(1, std::memory_order_release);
      wakeControl();
    }
    if (overflow || obsolete) return;
  }
}

static bool readAudio(float &sample, int64_t dtUs) {
  int64_t lastUs;
  uint32_t fill = audioFill(&lastUs);
  uint32_t fs = sampleRate.load();
  int64_t age = esp_timer_get_time() - lastUs;
  if (!connected.load() || !playing.load() || lastUs == 0 ||
      age < 0 || age > DATA_TIMEOUT_US || (fs != 44100 && fs != 48000)) {
    primed = false;
    return false;
  }
  if (!primed) {
    if (fill < PREFILL) return false;
    primed = true;
    resampleFraction = 0;
  }
  // Keep clock correction small to avoid audible pitch changes.
  float correction = clampf((float(fill) - TARGET_FILL) * 0.000003f,
                            -0.005f, 0.005f);
  float step = float(fs) * float(dtUs) * 0.000001f * (1.0f + correction);
  float nextFraction = resampleFraction + step;
  uint32_t consume = uint32_t(nextFraction);
  bool valid = false;
  portENTER_CRITICAL(&audioMux);
  fill = (rbHead - rbTail) & RB_MASK;
  if (fill >= consume + 2) {
    float a = rb[rbTail];
    float b = rb[(rbTail + 1) & RB_MASK];
    sample = (a + (b - a) * resampleFraction) / 32768.0f;
    rbTail = (rbTail + consume) & RB_MASK;
    valid = true;
  }
  portEXIT_CRITICAL(&audioMux);
  if (valid) resampleFraction = nextFraction - consume;
  else primed = false;
  return valid;
}

static uint32_t allowedCounts() {
  if (controlFault || !pwmReady || carrierReal == 0 ||
      !isfinite(TRANSFORMER_MAX_ON_US) || TRANSFORMER_MAX_ON_US <= 0) return 0;
  // Round down so quantization cannot lengthen the requested pulse.
  double byTime = floor(double(TRANSFORMER_MAX_ON_US) * carrierReal *
                        PWM_STEPS / 1000000.0);
  double byDuty = floor(double(min(maxDuty, DUTY_HARD_CEILING)) * PWM_STEPS);
  return uint32_t(fmax(0.0, fmin(double(PWM_STEPS - 1), fmin(byTime, byDuty))));
}

static void hardLowFallback() {
  ledcDetach(PIN_PWM);
  pinMode(PIN_PWM, OUTPUT);
  digitalWrite(PIN_PWM, LOW);
  pwmReady = false;
  armed = false;
  dutyApplied = 0;
  lastReason = "PWM API failure; restart required";
}

static bool writeDuty(uint32_t count) {
  if (!pwmReady) return false;
  count = min(count, allowedCounts());
  if (!ledcWrite(PIN_PWM, count)) {
    hardLowFallback();
    return false;
  }
  dutyApplied = count;
  return true;
}

static void disarm(const char *reason, bool flush = true) {
  armed = false;
  ramp = 0;
  phase = 0;
  primed = false;
  resampleFraction = 0;
  lastReason = reason;
  if (pwmReady) {
    if (!ledcWrite(PIN_PWM, 0)) hardLowFallback();
    else dutyApplied = 0;
  }
  if (flush) resetAudio();
}

static bool configureCarrier(uint32_t requested) {
  if (armed || !pwmReady) return false;
  if (!ledcWrite(PIN_PWM, 0)) { hardLowFallback(); return false; }
  uint32_t actual = ledcChangeFrequency(PIN_PWM, requested, PWM_BITS);
  if (!actual || !ledcWrite(PIN_PWM, 0)) { hardLowFallback(); return false; }
  carrierRequested = requested;
  carrierReal = actual;
  dutyApplied = 0;
  ramp = 0;
  return true;
}

static void publishStatus() {
  Status s;
  s.armed = armed; s.pwmReady = pwmReady;
  s.connected = connected.load(); s.playing = playing.load(); s.mode = mode;
  s.watchdogReady = watchdogReady; s.controlFault = controlFault;
  s.carrier = carrierReal; s.fill = audioFill(); s.fs = sampleRate.load();
  s.duty = dutyApplied; s.maxCount = allowedCounts();
  s.maxDuty = maxDuty; s.bias = bias; s.gain = gain; s.ramp = ramp;
  s.sqrtMap = sqrtMap; s.maxDtUs = maxDtUs; s.trips = deadlineTrips;
  maxDtUs = 0;
  s.reason = lastReason;
  xQueueOverwrite(statusQueue, &s);
}

static void handleCommand(char c) {
  if (c == 'x') { disarm("STOP"); return; }
  if (c == '?') return;
  if (c == 'a') {
    if (controlFault) {
      lastReason = "ARM blocked: control fault; restart required";
      return;
    }
    if (armed) { lastReason = "already armed"; return; }
    if (!pwmReady || allowedCounts() == 0) {
      lastReason = "ARM blocked: configure transformer pulse limit";
      return;
    }
    if (mode == MODE_BT) {
      int64_t lastUs;
      uint32_t fill = audioFill(&lastUs);
      if (!connected.load() || !playing.load() || !primed || fill < 16 ||
          lastUs == 0 || esp_timer_get_time() - lastUs > DATA_TIMEOUT_US) {
        lastReason = "ARM blocked: wait for active buffered Bluetooth audio";
        return;
      }
    }
    armed = true;
    ramp = 0;
    lastReason = "armed; soft start";
    return;
  }
  if (armed) { lastReason = "setting rejected: send x first"; return; }
  switch (c) {
    case 'b': case 't': case 'c':
      acceptAudio.store(false);
      disarm("mode changed");
      mode = c == 'b' ? MODE_BT : (c == 't' ? MODE_TONE : MODE_CONST);
      acceptAudio.store(mode == MODE_BT);
      break;
    case 'f': configureCarrier(max(FC_MIN, carrierRequested - 500)); break;
    case 'F': configureCarrier(min(FC_MAX, carrierRequested + 500)); break;
    case 'd': maxDuty = clampf(maxDuty - 0.01f, 0, DUTY_HARD_CEILING); break;
    case 'D': maxDuty = clampf(maxDuty + 0.01f, 0, DUTY_HARD_CEILING); break;
    case 'p': bias = clampf(bias - 0.05f, 0, 1); break;
    case 'P': bias = clampf(bias + 0.05f, 0, 1); break;
    case 'g': gain = clampf(gain - 0.05f, 0, 1); break;
    case 'G': gain = clampf(gain + 0.05f, 0, 1); break;
    case 's': sqrtMap = !sqrtMap; break;
    default: lastReason = "unknown command"; break;
  }
}

static void timerTick(void *) { wakeControl(); }

static void controlTask(void *) {
  watchdogReady = esp_task_wdt_add(nullptr) == ESP_OK;
  if (!watchdogReady) {
    controlFault = true;
    disarm("watchdog registration failed; restart required");
    publishStatus();
  }
  int64_t previousUpdate = esp_timer_get_time();
  int64_t previousStatus = 0;
  uint32_t epoch = sourceEpoch.load(std::memory_order_acquire);
  for (;;) {
    uint32_t notifications = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (watchdogReady && esp_task_wdt_reset() != ESP_OK) {
      watchdogReady = false;
      controlFault = true;
      disarm("watchdog feed failed; restart required");
      publishStatus();
    }
    int64_t now = esp_timer_get_time();
    int64_t dt = now - previousUpdate;
    previousUpdate = now;
    if (dt > maxDtUs) maxDtUs = dt;
    bool report = false, skipCommands = false;
    if (stopRequested.exchange(false)) {
      disarm("STOP");
      xQueueReset(commandQueue);
      skipCommands = true;
      report = true;
    }
    uint32_t currentEpoch = sourceEpoch.load(std::memory_order_acquire);
    if (epoch != currentEpoch) {
      epoch = currentEpoch;
      primed = false;
      resampleFraction = 0;
      if (mode == MODE_BT && armed) {
        disarm("Bluetooth state/rate change or buffer overflow");
        report = true;
        skipCommands = true;
        xQueueReset(commandQueue);
      }
    }
    if (armed && (dt > CONTROL_DEADLINE_US || notifications > 16)) {
      disarm("control task missed deadline");
      ++deadlineTrips;
      xQueueReset(commandQueue);
      skipCommands = true;
      report = true;
    }
    // Use elapsed time: commands and Bluetooth can also wake this task.
    float audio = 0;
    bool audioValid = true;
    if (mode == MODE_BT) {
      audioValid = readAudio(audio, dt);
      if (!audioValid && armed) {
        disarm("Bluetooth audio unavailable/underrun");
        xQueueReset(commandQueue);
        skipCommands = true;
        report = true;
      }
    } else if (mode == MODE_TONE) {
      phase += 440.0f * float(dt) * 0.000001f;
      phase -= floorf(phase);
      audio = sinf(2.0f * PI * phase);
    }
    char c;
    if (!skipCommands) {
      for (int i = 0; i < 4 && xQueueReceive(commandQueue, &c, 0) == pdTRUE; ++i) {
        handleCommand(c);
        report = true;
      }
    }
    // Recheck after processing commands; the source may have changed.
    if (mode == MODE_BT && sourceEpoch.load(std::memory_order_acquire) != epoch) {
      disarm("Bluetooth source changed during update");
      report = true;
    } else if (stopRequested.load()) {
      disarm("STOP");
    } else if (armed && audioValid) {
      ramp = min(1.0f, ramp + float(dt) / SOFT_START_US);
      float level = clampf(bias + gain * audio, 0, 1);
      if (sqrtMap) level = sqrtf(level);
      uint32_t count = uint32_t(floorf(allowedCounts() * level * ramp));
      if (!writeDuty(count)) report = true;
    } else if (dutyApplied != 0) {
      writeDuty(0);
    }
    if (report || now - previousStatus >= 1000000) {
      publishStatus();
      previousStatus = now;
    }
  }
}

static void fatalSetup(const char *message) {
  if (controlTaskHandle) {
    esp_task_wdt_delete(controlTaskHandle);
    vTaskDelete(controlTaskHandle);
    controlTaskHandle = nullptr;
  }
  if (pwmReady) ledcDetach(PIN_PWM);
  pinMode(PIN_PWM, OUTPUT);
  digitalWrite(PIN_PWM, LOW);
  Serial.println(message);
  for (;;) delay(1000);
}

void setup() {
  pinMode(PIN_PWM, OUTPUT);
  digitalWrite(PIN_PWM, LOW);
  Serial.begin(115200);
  commandQueue = xQueueCreate(16, sizeof(char));
  statusQueue = xQueueCreate(1, sizeof(Status));
  if (!commandQueue || !statusQueue) fatalSetup("Queue allocation failed");
  pwmReady = ledcAttach(PIN_PWM, carrierRequested, PWM_BITS);
  if (!pwmReady) fatalSetup("LEDC attach failed");
  if (!ledcWrite(PIN_PWM, 0)) fatalSetup("LEDC zero-duty failed");
  carrierReal = ledcReadFreq(PIN_PWM);
  if (!carrierReal) fatalSetup("LEDC frequency read failed");
  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms = WDT_TIMEOUT_MS;
  wdtCfg.idle_core_mask = 0;
  wdtCfg.trigger_panic = true;
  esp_err_t wdtErr = esp_task_wdt_reconfigure(&wdtCfg);
  if (wdtErr == ESP_ERR_INVALID_STATE) wdtErr = esp_task_wdt_init(&wdtCfg);
  if (wdtErr != ESP_OK) fatalSetup("Watchdog setup failed");
  if (xTaskCreatePinnedToCore(controlTask, "gromofon-control", 6144, nullptr,
                            4, &controlTaskHandle, 1) != pdPASS) {
    fatalSetup("Control task creation failed");
  }
  esp_timer_create_args_t args = {};
  args.callback = timerTick;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "gromofon-update";
  args.skip_unhandled_events = true;
  if (esp_timer_create(&args, &updateTimer) != ESP_OK ||
      esp_timer_start_periodic(updateTimer, UPDATE_US) != ESP_OK) {
    fatalSetup("Update timer initialization failed");
  }
  a2dp_sink.set_on_connection_state_changed(onConnection);
  a2dp_sink.set_on_audio_state_changed(onAudioState);
  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.set_stream_reader(onStreamData, false);
  a2dp_sink.start("Gromofon");
  Serial.println("Gromofon v0.3. DISARMED. ?=status, x=stop.");
#if BENCH_NO_POWER_STAGE
  Serial.println("BENCH MODE: test pulse limit active. Power stage must be DISCONNECTED.");
#else
  if (TRANSFORMER_MAX_ON_US <= 0)
    Serial.println("No validated transformer pulse limit configured: ARM is blocked.");
#endif
}

void loop() {
  for (int i = 0; i < 16 && Serial.available(); ++i) {
    char c = char(Serial.read());
    if (c == '\r' || c == '\n' || c == ' ') continue;
    if (c == 'x') {
      stopRequested.store(true);
      xQueueReset(commandQueue);
      wakeControl();
    } else if (xQueueSend(commandQueue, &c, 0) != pdTRUE) {
      stopRequested.store(true);
      wakeControl();
      Serial.println("Command queue full: stop requested");
    }
  }
  Status s;
  if (xQueueReceive(statusQueue, &s, 0) == pdTRUE) {
    const char *name = s.mode == MODE_BT ? "BT" : (s.mode == MODE_TONE ? "TONE" : "CONST");
    Serial.printf("%sarmed=%d pwmOK=%d wdt=%d fault=%d mode=%s map=%s fc=%luHz duty=%lu/%u cap=%lu "
                  "dmax=%.2f bias=%.2f gain=%.2f ramp=%.2f bt=%d play=%d "
                  "fs=%lu fill=%lu maxdt=%lldus trips=%lu | %s\n",
                  BENCH_NO_POWER_STAGE ? "[BENCH] " : "",
                  int(s.armed), int(s.pwmReady), int(s.watchdogReady), int(s.controlFault),
                  name, s.sqrtMap ? "sqrt" : "lin",
                  (unsigned long)s.carrier,
                  (unsigned long)s.duty, unsigned(PWM_STEPS), (unsigned long)s.maxCount,
                  s.maxDuty, s.bias, s.gain, s.ramp, int(s.connected), int(s.playing),
                  (unsigned long)s.fs, (unsigned long)s.fill,
                  (long long)s.maxDtUs, (unsigned long)s.trips, s.reason);
  }
  delay(1);
}

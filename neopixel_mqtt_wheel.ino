/*
 * ============================================================
 *  NeoPixel MQTT Controller — Wemos D1 Mini (ESP8266)
 *  WHEEL MODE = animated white <-> ink-blue cycle
 * ============================================================
 *  Flow:  panel  ->  HiveMQ broker  ->  this ESP  ->  strip
 *
 *  Payload from panel:  mode,hex1,hex2,brightness,speed
 *  e.g.                 wheel,ff29c6,3e2aff,150,40
 *
 *  Wiring (IMPORTANT):
 *    D2 on the board = GPIO4  -> LED_PIN must be 4, NOT 0
 *    GPIO0 is the boot pin and will not drive LEDs reliably.
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>

// ======================= USER CONFIG ========================
const char* WIFI_SSID   = "Rapid";
const char* WIFI_PASS   = "Vizwhack134";

const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT = "esp8266-satha-neopixel-7734";

const char* TOPIC_SUB   = "home/neopixel/set";
const char* TOPIC_PUB   = "home/neopixel/state";

// ── wheel mode colours ──────────────────────────────────────
#define C1_R 150   // white
#define C1_G 150
#define C1_B 150

#define C2_R 255    // ink blue
#define C2_G 0
#define C2_B 0

#define LED_PIN   D2     // ← FIXED: D2 = GPIO4 on Wemos D1 Mini (was 0)
#define NUM_LEDS  8
// ============================================================

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiClient   wifi;
PubSubClient mqtt(wifi);

// ── light state ─────────────────────────────────────────────
enum Mode { WHEEL, GRADIENT, RAINBOW, OFF };   // ← uppercase, no name clash
Mode     mode       = WHEEL;
uint8_t  r1 = 255, g1 = 124, b1 = 42;
uint8_t  r2 = 62,  g2 = 42,  b2 = 255;
uint8_t  brightness = 150;
uint8_t  speedVal   = 40;
uint16_t hueOffset  = 0;
unsigned long lastFrame   = 0;
unsigned long lastMqttTry = 0;

// ── colour helpers ───────────────────────────────────────────
uint8_t lerp8(uint8_t a, uint8_t b, float t) {
  return (uint8_t)(a + (b - a) * t);
}

bool hexToRGB(String h, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (h.length() != 6) return false;
  long v = strtol(h.c_str(), nullptr, 16);
  r = (v >> 16) & 0xFF;  g = (v >> 8) & 0xFF;  b = v & 0xFF;
  return true;
}

// Smooth 0..255 sine that WRAPS cleanly over a 256-step input.
// (theta/256 * 2*PI means theta=0 and theta=256 line up — no jump.)
uint8_t sin8(uint8_t theta) {
  float rad = (theta / 256.0f) * 2.0f * PI;
  return (uint8_t)((sin(rad) + 1.0f) * 127.5f);
}

// ── rendering ────────────────────────────────────────────────
void renderWheel() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t phase = (hueOffset + (uint32_t)i * 65536UL / NUM_LEDS) & 0xFFFF;
    uint8_t  t = sin8(phase >> 8);               // 0..255 wave
    uint8_t  r = map(t, 0, 255, C1_R, C2_R);
    uint8_t  g = map(t, 0, 255, C1_G, C2_G);
    uint8_t  b = map(t, 0, 255, C1_B, C2_B);
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
}

void renderGradient() {
  for (int i = 0; i < NUM_LEDS; i++) {
    float t = (NUM_LEDS > 1) ? (float)i / (NUM_LEDS - 1) : 0;
    strip.setPixelColor(i, strip.Color(lerp8(r1, r2, t),
                                       lerp8(g1, g2, t),
                                       lerp8(b1, b2, t)));
  }
}

void renderRainbow() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t hue = hueOffset + (uint32_t)i * 65536UL / NUM_LEDS;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue)));
  }
}

void applyState() {
  strip.setBrightness(brightness);
  switch (mode) {
    case WHEEL:    renderWheel();    break;
    case GRADIENT: renderGradient(); break;
    case RAINBOW:  renderRainbow();  break;
    default:       strip.clear();    break;
  }
  strip.show();
}

// ── parse payload:  mode,hex1,hex2,brightness,speed ──────────
void parsePayload(String msg) {
  Serial.println("Received: " + msg);

  String parts[5];
  int idx = 0; String tok;
  for (unsigned int i = 0; i <= msg.length() && idx < 5; i++) {
    char c = (i < msg.length()) ? msg[i] : ',';
    if (c == ',') { parts[idx++] = tok; tok = ""; }
    else tok += c;
  }
  if (idx < 5) { Serial.println("  bad payload (need 5 fields)"); return; }

  String m = parts[0]; m.toLowerCase();
  if      (m == "wheel")    mode = WHEEL;
  else if (m == "gradient") mode = GRADIENT;
  else if (m == "rainbow")  mode = RAINBOW;
  else                      mode = OFF;

  hexToRGB(parts[1], r1, g1, b1);
  hexToRGB(parts[2], r2, g2, b2);
  brightness = constrain(parts[3].toInt(), 0, 255);
  speedVal   = constrain(parts[4].toInt(), 1, 100);

  applyState();
  mqtt.publish(TOPIC_PUB, msg.c_str());
}

void onMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  parsePayload(msg);
}

// ── WiFi (never reboots) ─────────────────────────────────────
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("\nWiFi: connecting to \"%s\"\n", WIFI_SSID);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
    delay(500);
    Serial.printf("  status=%d\n", WiFi.status());
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("WiFi OK: " + WiFi.localIP().toString());
  else
    Serial.println("WiFi not up yet — retrying (no reboot)");
}

// ── MQTT (one try / 5 s) ─────────────────────────────────────
void connectMQTT() {
  if (lastMqttTry != 0 && millis() - lastMqttTry < 5000) return;
  lastMqttTry = millis();
  Serial.print("MQTT: connecting to "); Serial.print(MQTT_SERVER); Serial.print(" … ");
  if (mqtt.connect(MQTT_CLIENT)) {
    Serial.println("connected!");
    mqtt.subscribe(TOPIC_SUB, 1);
    Serial.println("Subscribed to: " + String(TOPIC_SUB));
  } else {
    Serial.printf("failed rc=%d (retry in 5 s)\n", mqtt.state());
  }
}

// ── setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  strip.begin();
  brightness = 40; r1 = 255; g1 = 80; b1 = 0;   // dim orange = alive
  applyState();
  connectWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(512);
}

// ── loop ─────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return; }
  if (!mqtt.connected())              { connectMQTT(); }
  mqtt.loop();

  // ── FIXED: animate BOTH wheel and rainbow here ──
  if (mode == WHEEL || mode == RAINBOW) {
    uint16_t interval = map(speedVal, 1, 100, 120, 8);  // ms per frame
    if (millis() - lastFrame >= interval) {
      lastFrame = millis();
      hueOffset += 4096;                 // advance the animation
      if (mode == WHEEL) renderWheel();
      else               renderRainbow();
      strip.show();
    }
  }
}

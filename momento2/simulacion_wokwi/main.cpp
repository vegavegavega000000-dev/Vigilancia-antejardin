#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include "mbedtls/md.h"
const char* CLAVE = "clave-secreta-ucc7788";

#define PIN_PIR 2
#define PIN_TRIG 3
#define PIN_ECHO 10
#define PIN_BTN_MALO 4
#define PIN_SW_CAIDO 5

const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
const char* TOPIC_AVISO  = "ucc7788/casa/antejardin/aviso";
const char* TOPIC_ESTADO = "ucc7788/casa/antejardin/estado";
const char* ID = "antejardin-n1";
const int   VERSION = 1;

WiFiClient red;
PubSubClient mqtt(red);

String ahora() {
  struct tm t;
  if (!getLocalTime(&t, 50) || t.tm_year < 124) return "";
  char s[32];
  strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%S-05:00", &t);
  return String(s);
}

// Devuelve -1 si se perdió el eco: "sin dato no es cero"
// Devuelve -1 si se perdió el eco: "sin dato no es cero"
float leerCm() {
  digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long t0 = micros();
  while (digitalRead(PIN_ECHO) == LOW) {
    if (micros() - t0 > 30000) return -1.0;
  }
  unsigned long ini = micros();
  while (digitalRead(PIN_ECHO) == HIGH) {
    if (micros() - ini > 30000) return -1.0;
  }
  return (micros() - ini) / 58.0;
}

void reconectar() {
  while (!mqtt.connected()) {
    // Last Will: si el ESP32 se desconecta, el broker avisa "offline"
    if (mqtt.connect("ucc7788-antejardin-esp32c3", nullptr, nullptr, TOPIC_ESTADO, 0, true, "offline")) {
      mqtt.publish(TOPIC_ESTADO, "online", true);
    } else {
      delay(1000);
    }
  }
}

String firmar(const String& texto) {
  byte h[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)CLAVE, strlen(CLAVE));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)texto.c_str(), texto.length());
  mbedtls_md_hmac_finish(&ctx, h);
  mbedtls_md_free(&ctx);
  String s; char b[3];
  for (int i = 0; i < 32; i++) { sprintf(b, "%02x", h[i]); s += b; }
  return s;
}

void publicarLectura(bool malo) {
  bool mov = digitalRead(PIN_PIR);
  float d = leerCm();
  String m = ahora();
  String movS = mov ? "true" : "false";
  String dist = d < 0 ? "null" : String(d, 1);
  String canon = String(ID) + "|" + movS + "|" + (malo ? "" : dist) + "|cm|" + m + "|" + VERSION;
  String firma = firmar(canon);
  char msg[360];
  if (malo) {
    snprintf(msg, sizeof(msg),
      "{\"id\":\"%s\",\"movimiento\":%s,\"unidad\":\"cm\",\"momento\":\"%s\",\"version\":%d,\"firma\":\"%s\"}",
      ID, movS.c_str(), m.c_str(), VERSION, firma.c_str());
  } else {
    snprintf(msg, sizeof(msg),
      "{\"id\":\"%s\",\"movimiento\":%s,\"distancia\":%s,\"unidad\":\"cm\",\"momento\":\"%s\",\"version\":%d,\"firma\":\"%s\"}",
      ID, movS.c_str(), dist.c_str(), m.c_str(), VERSION, firma.c_str());
  }
  Serial.println(msg);
  mqtt.publish(TOPIC_AVISO, msg);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BTN_MALO, INPUT_PULLUP);
  pinMode(PIN_SW_CAIDO, INPUT);
  WiFi.begin("Wokwi-GUEST", "", 6);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  configTime(-5 * 3600, 0, "pool.ntp.org");
    struct tm t;
  while (!getLocalTime(&t, 500) || t.tm_year < 124) Serial.println("Esperando hora NTP...");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  Serial.println("WiFi OK");
}

void loop() {
  reconectar();
  mqtt.loop();

  // PRUEBA 3: sensor caído -> el ESP32 deja de publicar
  if (digitalRead(PIN_SW_CAIDO) == HIGH) return;

  static bool btnPrev = HIGH;
  bool btn = digitalRead(PIN_BTN_MALO);
  if (btnPrev == HIGH && btn == LOW) publicarLectura(true);
  btnPrev = btn;

  static unsigned long t = 0;
  if (millis() - t >= 2000) { t = millis(); publicarLectura(false); }
}

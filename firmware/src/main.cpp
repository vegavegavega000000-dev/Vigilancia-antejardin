// Firmware de monitoreo local: radar LD2410C + TF-Luna -> lineas JSON por USB serial
// Lo consume el dashboard local (dashboard/servidor.py).
// Diagnosticos anteriores en firmware/diagnosticos/: diag_radar_ld2410c.cpp, diag_tfluna.cpp
//
// Comandos que acepta por serial (una linea): "chequeo", "info"
#include <Arduino.h>
#include <esp_system.h>

#define FW_VERSION "monitor-1"

// ---- Pines confirmados por diagnostico (2026-11-12) ----
const int RADAR_RX = 5;    // radar TX  -> GPIO5
const int RADAR_TX = 6;    // radar RX  <- GPIO6
const int RADAR_OUT = 4;   // radar OUT -> GPIO4
const int LUNA_RX = 20;    // TF-Luna TXD -> GPIO20
const int LUNA_TX = 21;    // TF-Luna RXD <- GPIO21
const long RADAR_BAUD = 256000;
const long LUNA_BAUD = 115200;

HardwareSerial& Radar = Serial1;   // UART1
HardwareSerial& Luna = Serial0;    // UART0 (Serial es el USB)

// ---- Comandos ----
const uint8_t RADAR_CFG_ON[]  = {0xFD,0xFC,0xFB,0xFA, 0x04,0x00, 0xFF,0x00, 0x01,0x00, 0x04,0x03,0x02,0x01};
const uint8_t RADAR_CFG_OFF[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0xFE,0x00, 0x04,0x03,0x02,0x01};
const uint8_t LUNA_VERSION[]  = {0x5A, 0x04, 0x01, 0x5F};

// ---- Estado ----
struct {
  int estado = 0, movD = 0, movE = 0, estD = 0, estE = 0, detD = 0;
  unsigned long tUltima = 0; int tramasSeg = 0, fps = 0; int reconex = 0;
  bool ack = false;
} rad;

struct {
  int dist = 0, amp = 0; float temp = 0;
  unsigned long tUltima = 0; int tramasSeg = 0, fps = 0; int reconex = 0, malas = 0;
  char version[16] = "";
} lun;

const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "encendido";
    case ESP_RST_EXT:       return "externo";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "fallo de codigo (panic)";
    case ESP_RST_INT_WDT:   return "watchdog de interrupcion";
    case ESP_RST_TASK_WDT:  return "watchdog de tarea";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "sueno profundo";
    case ESP_RST_BROWNOUT:  return "caida de voltaje (brownout)";
    default: {
      static char txt[40];
      snprintf(txt, sizeof(txt), "otro (codigo %d, p. ej. carga por USB)", (int)esp_reset_reason());
      return txt;
    }
  }
}

// ---------- Parser del radar (tramas de datos F4F3F2F1 y ACK FDFCFBFA) ----------
uint8_t rb[64]; int rn = 0; bool rEsAck = false;
const uint8_t H_DATA[] = {0xF4,0xF3,0xF2,0xF1}, H_ACK[] = {0xFD,0xFC,0xFB,0xFA};

void radarTrama() {
  int len = rb[4] | (rb[5] << 8);
  uint8_t* p = rb + 6;
  if (rEsAck) {
    // ACK de "habilitar configuracion": FF 01 00 00 ...
    if (len >= 4 && p[0] == 0xFF && p[1] == 0x01 && p[2] == 0 && p[3] == 0) rad.ack = true;
    return;
  }
  if (len < 11 || p[1] != 0xAA) return;
  rad.estado = p[2];
  rad.movD = p[3] | (p[4] << 8);  rad.movE = p[5];
  rad.estD = p[6] | (p[7] << 8);  rad.estE = p[8];
  rad.detD = p[9] | (p[10] << 8);
  rad.tUltima = millis(); rad.tramasSeg++;
}

void radarByte(uint8_t b) {
  if (rn < 4) {
    if (rn == 0) {
      if (b == H_DATA[0]) rEsAck = false;
      else if (b == H_ACK[0]) rEsAck = true;
      else return;
    } else if (b != (rEsAck ? H_ACK : H_DATA)[rn]) {
      rn = 0; radarByte(b); return;
    }
    rb[rn++] = b; return;
  }
  rb[rn++] = b;
  if (rn == 6) {
    int len = rb[4] | (rb[5] << 8);
    if (len > 48) { rn = 0; return; }
  }
  if (rn >= 6) {
    int len = rb[4] | (rb[5] << 8);
    if (rn == 6 + len + 4) { radarTrama(); rn = 0; }
  }
}

// ---------- Parser del TF-Luna (datos 59 59 ... y respuestas 5A ...) ----------
uint8_t lb[32]; int ln = 0; bool lEsCmd = false;

void lunaByte(uint8_t b) {
  if (ln == 0) {
    if (b == 0x59) lEsCmd = false;
    else if (b == 0x5A) lEsCmd = true;
    else return;
    lb[ln++] = b; return;
  }
  if (!lEsCmd) {
    if (ln == 1 && b != 0x59) { ln = 0; lunaByte(b); return; }
    lb[ln++] = b;
    if (ln == 9) {
      ln = 0;
      uint8_t s = 0; for (int i = 0; i < 8; i++) s += lb[i];
      if (s != lb[8]) { lun.malas++; return; }
      lun.dist = lb[2] | (lb[3] << 8);
      lun.amp  = lb[4] | (lb[5] << 8);
      lun.temp = (lb[6] | (lb[7] << 8)) / 8.0f - 256.0f;
      lun.tUltima = millis(); lun.tramasSeg++;
    }
  } else {
    if (ln == 1 && (b < 4 || b > 30)) { ln = 0; return; }
    lb[ln++] = b;
    if (ln == lb[1]) {
      int n = ln; ln = 0;
      uint8_t s = 0; for (int i = 0; i < n - 1; i++) s += lb[i];
      if (s != lb[n - 1]) return;
      if (lb[2] == 0x01 && n >= 7) snprintf(lun.version, sizeof(lun.version), "%d.%d.%d", lb[5], lb[4], lb[3]);
    }
  }
}

// ---------- Utilidades ----------
void abrirRadar() { Radar.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX, RADAR_TX); }
void abrirLuna()  { Luna.begin(LUNA_BAUD, SERIAL_8N1, LUNA_RX, LUNA_TX); }

void leerSensores() {
  while (Radar.available()) radarByte(Radar.read());
  while (Luna.available())  lunaByte(Luna.read());
}

void esperar(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) { leerSensores(); delay(1); }
}

void emitirInfo() {
  Serial.printf("{\"tipo\":\"info\",\"fw\":\"%s\",\"reinicio\":\"%s\",\"uptime\":%lu,"
                "\"radarAck\":%s,\"lunaVersion\":\"%s\",\"reconexRadar\":%d,\"reconexLuna\":%d,\"lunaMalas\":%d}\n",
                FW_VERSION, motivoReinicio(), millis() / 1000,
                rad.ack ? "true" : "false", lun.version, rad.reconex, lun.reconex, lun.malas);
}

void chequeo() {
  rad.ack = false; lun.version[0] = 0;
  for (int k = 0; k < 3 && (!rad.ack || !lun.version[0]); k++) {
    if (!rad.ack) { Radar.write(RADAR_CFG_ON, sizeof(RADAR_CFG_ON)); }
    if (!lun.version[0]) { Luna.write(LUNA_VERSION, sizeof(LUNA_VERSION)); }
    esperar(300);
    Radar.write(RADAR_CFG_OFF, sizeof(RADAR_CFG_OFF));
    esperar(100);
  }
  Serial.printf("{\"tipo\":\"chequeo\",\"radarAck\":%s,\"lunaVersion\":\"%s\"}\n",
                rad.ack ? "true" : "false", lun.version);
  emitirInfo();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // no bloquear si el PC no esta leyendo
  pinMode(RADAR_OUT, INPUT_PULLDOWN);
  abrirRadar();
  abrirLuna();
  esperar(800);
  // UART0 a veces no recibe tras el primer begin(): reabrir si no llego nada
  if (lun.tUltima == 0) { Luna.end(); abrirLuna(); esperar(500); }
  Serial.printf("{\"tipo\":\"inicio\",\"fw\":\"%s\",\"reinicio\":\"%s\"}\n", FW_VERSION, motivoReinicio());
  chequeo();
}

String linea;
void loop() {
  leerSensores();
  unsigned long ahora = millis();

  // Reabrir un UART si su sensor deja de mandar tramas
  static unsigned long tWatch = 0;
  if (ahora - tWatch > 1000) {
    tWatch = ahora;
    if (ahora - rad.tUltima > 3000) { Radar.end(); abrirRadar(); rad.reconex++; }
    if (ahora - lun.tUltima > 3000) { Luna.end();  abrirLuna();  lun.reconex++; }
  }

  // Tramas por segundo
  static unsigned long tFps = 0;
  if (ahora - tFps >= 1000) {
    tFps = ahora;
    rad.fps = rad.tramasSeg; rad.tramasSeg = 0;
    lun.fps = lun.tramasSeg; lun.tramasSeg = 0;
  }

  // Dato cada 200 ms
  static unsigned long tDato = 0;
  if (ahora - tDato >= 200) {
    tDato = ahora;
    bool rOk = rad.tUltima && ahora - rad.tUltima < 1500;
    bool lOk = lun.tUltima && ahora - lun.tUltima < 1500;
    Serial.printf("{\"tipo\":\"dato\",\"ms\":%lu,"
                  "\"radar\":{\"ok\":%s,\"fps\":%d,\"estado\":%d,\"movD\":%d,\"movE\":%d,\"estD\":%d,\"estE\":%d,\"detD\":%d,\"out\":%d},"
                  "\"luna\":{\"ok\":%s,\"fps\":%d,\"dist\":%d,\"amp\":%d,\"temp\":%.1f}}\n",
                  ahora,
                  rOk ? "true" : "false", rad.fps, rad.estado, rad.movD, rad.movE, rad.estD, rad.estE, rad.detD,
                  digitalRead(RADAR_OUT),
                  lOk ? "true" : "false", lun.fps, lun.dist, lun.amp, lun.temp);
  }

  // Info cada 5 s (por si el dashboard se conecto tarde)
  static unsigned long tInfo = 0;
  if (ahora - tInfo >= 5000) { tInfo = ahora; emitirInfo(); }

  // Comandos desde el PC
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      linea.trim();
      if (linea == "chequeo") chequeo();
      else if (linea == "info") emitirInfo();
      linea = "";
    } else if (linea.length() < 32) linea += c;
  }
}

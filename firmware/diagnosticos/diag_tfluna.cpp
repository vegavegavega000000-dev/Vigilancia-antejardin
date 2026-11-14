// Diagnostico del TF-Luna (UART) en ESP32-C3 SuperMini
// Diagnostico del radar guardado en firmware/diagnosticos/diag_radar_ld2410c.cpp
// Trama TF-Luna (9 bytes): 59 59 DistL DistH AmpL AmpH TempL TempH Checksum
#include <Arduino.h>

#include <esp_system.h>
HardwareSerial& Luna = Serial1;   // UART1 para la prueba (ya probada con el radar)

// Comando "obtener version": 5A 04 01 5F -> respuesta 5A 07 01 ...
const uint8_t CMD_VERSION[] = {0x5A, 0x04, 0x01, 0x5F};

int rxBueno = -1, txBueno = -1;
long baudBueno = 0;

void revisarPin(int p) {
  int altosPD = 0, cambiosPD = 0, altosPU = 0, cambiosPU = 0;
  pinMode(p, INPUT_PULLDOWN); delay(5);
  int ant = digitalRead(p);
  for (int i = 0; i < 20000; i++) {
    int v = digitalRead(p); if (v) altosPD++; if (v != ant) cambiosPD++; ant = v; delayMicroseconds(10);
  }
  pinMode(p, INPUT_PULLUP); delay(5);
  ant = digitalRead(p);
  for (int i = 0; i < 20000; i++) {
    int v = digitalRead(p); if (v) altosPU++; if (v != ant) cambiosPU++; ant = v; delayMicroseconds(10);
  }
  pinMode(p, INPUT);
  Serial.printf("  GPIO%d  pulldown: %3d%% alto, %5d cambios | pullup: %3d%% alto, %5d cambios  -> ",
                p, altosPD * 100 / 20000, cambiosPD, altosPU * 100 / 20000, cambiosPU);
  if (cambiosPD > 20 || cambiosPU > 20)         Serial.println("ACTIVIDAD (TX del TF-Luna?)");
  else if (altosPD > 19900 && altosPU > 19900)  Serial.println("FIJO EN ALTO (RX del TF-Luna con pull-up?)");
  else if (altosPD < 100 && altosPU < 100)      Serial.println("FIJO EN BAJO (GND?)");
  else if (altosPD < 100 && altosPU > 19900)    Serial.println("SIGUE AL PULL = parece SIN CONEXION");
  else                                          Serial.println("indefinido");
}

bool probar(int rx, int tx, long baud) {
  Luna.begin(baud, SERIAL_8N1, rx, tx);
  delay(50);
  while (Luna.available()) Luna.read();
  unsigned long t0 = millis();
  int bytes = 0, tramas = 0, malas = 0;
  uint8_t f[9]; int n = 0;
  while (millis() - t0 < 1000) {
    while (Luna.available()) {
      uint8_t b = Luna.read(); bytes++;
      if (n == 0 && b != 0x59) continue;
      if (n == 1 && b != 0x59) { n = 0; continue; }
      f[n++] = b;
      if (n == 9) {
        uint8_t s = 0; for (int i = 0; i < 8; i++) s += f[i];
        if (s == f[8]) tramas++; else malas++;
        n = 0;
      }
    }
  }
  // Comando de version (prueba ESP32 TX -> TF-Luna RX)
  int resp = 0;
  for (int k = 0; k < 3 && !resp; k++) {
    Luna.write(CMD_VERSION, sizeof(CMD_VERSION)); Luna.flush();
    t0 = millis(); uint8_t a = 0;
    while (millis() - t0 < 200) {
      while (Luna.available()) {
        uint8_t b = Luna.read();
        if (a == 0x5A && b == 0x07) resp++;
        a = b;
      }
    }
  }
  Luna.end();
  Serial.printf("  RX=GPIO%d TX=GPIO%d @%ld: %5d bytes, %4d tramas OK, %3d con error, respuesta a comando: %s\n",
                rx, tx, baud, bytes, tramas, malas, resp ? "SI" : "no");
  if (tramas > 0 && (rxBueno < 0 || resp)) { rxBueno = rx; txBueno = tx; baudBueno = baud; }
  return tramas > 0 && resp;
}

void diagnostico() {
  Serial.println();
  Serial.println("========== DIAGNOSTICO TF-LUNA ==========");
  Serial.println("[1] Nivel electrico (UART apagado):");
  revisarPin(20); revisarPin(21);
  Serial.println("[2] Probando UART:");
  rxBueno = -1;
  long bauds[] = {115200, 9600, 256000};
  int pares[][2] = {{20,21},{21,20}};
  for (long bd : bauds) for (auto &p : pares) if (probar(p[0], p[1], bd)) goto fin;
fin:
  if (rxBueno >= 0)
    Serial.printf("RESULTADO: TF-Luna TXD -> GPIO%d, RXD -> GPIO%d, %ld baudios\n", rxBueno, txBueno, baudBueno);
  else {
    Serial.println("RESULTADO: NO llegan tramas del TF-Luna por GPIO20 ni GPIO21.");
    Serial.println("  Revisar: +5V, GND, y que el pin 5 (config) NO este a GND (GND = modo I2C).");
  }
  Serial.println("=========================================");
}

void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 4000) delay(10);
  delay(1500);
  Serial.println("Diagnostico TF-Luna iniciado");
  const char* motivos[] = {"desconocido","encendido","externo","software","PANIC (fallo de codigo)",
                           "watchdog int","watchdog tarea","watchdog","sueno profundo","BROWNOUT (caida de voltaje)","SDIO","USB","JTAG"};
  int r = (int)esp_reset_reason();
  Serial.printf("Motivo del ultimo reinicio: %d = %s\n", r, r < 13 ? motivos[r] : "?");
}

unsigned long ultimoDiag = 0;
void loop() {
  if (ultimoDiag == 0 || millis() - ultimoDiag > 30000) {
    diagnostico();
    ultimoDiag = millis();
    if (rxBueno >= 0) Luna.begin(baudBueno, SERIAL_8N1, rxBueno, txBueno);
  }
  if (rxBueno < 0) return;
  static uint8_t f[9]; static int n = 0;
  static unsigned long tPrint = 0;
  while (Luna.available()) {
    uint8_t b = Luna.read();
    if (n == 0 && b != 0x59) continue;
    if (n == 1 && b != 0x59) { n = 0; continue; }
    f[n++] = b;
    if (n == 9) {
      n = 0;
      uint8_t s = 0; for (int i = 0; i < 8; i++) s += f[i];
      if (s != f[8]) continue;
      if (millis() - tPrint < 500) continue;
      tPrint = millis();
      int dist = f[2] | (f[3] << 8);
      int amp  = f[4] | (f[5] << 8);
      float temp = (f[6] | (f[7] << 8)) / 8.0 - 256.0;
      const char* nota = "";
      if (amp < 100 || amp == 65535) nota = "  <- intensidad sospechosa";
      else if (dist < 20 || dist > 800) nota = "  <- fuera de rango 20-800";
      Serial.printf("TF-Luna: dist=%4d cm  intensidad=%5d  temp=%.1f C%s\n", dist, amp, temp, nota);
    }
  }
}

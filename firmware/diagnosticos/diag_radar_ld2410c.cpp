// Diagnostico de cableado del radar LD2410C en ESP32-C3 SuperMini
// El codigo anterior (Wokwi / HC-SR04) esta respaldado en firmware/diagnosticos/main_wokwi_hcsr04.cpp
#include <Arduino.h>

const int PINES[] = {4, 5, 6};
const int NPINES = 3;

// Comando "habilitar configuracion" del LD2410 y "terminar configuracion"
const uint8_t CMD_ON[]  = {0xFD,0xFC,0xFB,0xFA, 0x04,0x00, 0xFF,0x00, 0x01,0x00, 0x04,0x03,0x02,0x01};
const uint8_t CMD_OFF[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0xFE,0x00, 0x04,0x03,0x02,0x01};

int rxBueno = -1, txBueno = -1;
long baudBueno = 0;

// Muestrea un pin con pull-down y con pull-up y cuenta cambios
void revisarPin(int p) {
  int altosPD = 0, cambiosPD = 0, altosPU = 0, cambiosPU = 0;
  pinMode(p, INPUT_PULLDOWN); delay(5);
  int ant = digitalRead(p);
  for (int i = 0; i < 20000; i++) {
    int v = digitalRead(p);
    if (v) altosPD++;
    if (v != ant) cambiosPD++;
    ant = v;
    delayMicroseconds(10);
  }
  pinMode(p, INPUT_PULLUP); delay(5);
  ant = digitalRead(p);
  for (int i = 0; i < 20000; i++) {
    int v = digitalRead(p);
    if (v) altosPU++;
    if (v != ant) cambiosPU++;
    ant = v;
    delayMicroseconds(10);
  }
  pinMode(p, INPUT);
  Serial.printf("  GPIO%d  pulldown: %3d%% alto, %5d cambios | pullup: %3d%% alto, %5d cambios  -> ",
                p, altosPD * 100 / 20000, cambiosPD, altosPU * 100 / 20000, cambiosPU);
  if (cambiosPD > 20 || cambiosPU > 20)         Serial.println("ACTIVIDAD (posible TX del radar)");
  else if (altosPD > 19900 && altosPU > 19900)  Serial.println("FIJO EN ALTO (radar OUT=presencia, o pin 3.3V!)");
  else if (altosPD < 100 && altosPU < 100)      Serial.println("FIJO EN BAJO (OUT sin presencia, o GND)");
  else if (altosPD < 100 && altosPU > 19900)    Serial.println("SIGUE AL PULL = parece SIN CONEXION (o RX del radar)");
  else                                          Serial.println("indefinido");
}

// Abre Serial1 con rx/tx, escucha, manda comando y busca respuesta
bool probarUart(int rx, int tx, long baud) {
  Serial1.begin(baud, SERIAL_8N1, rx, tx);
  delay(50);
  while (Serial1.available()) Serial1.read();

  // 1) escuchar tramas de datos (F4 F3 F2 F1)
  unsigned long t0 = millis();
  int bytes = 0, tramas = 0;
  uint8_t h[4] = {0};
  while (millis() - t0 < 1500) {
    while (Serial1.available()) {
      uint8_t b = Serial1.read(); bytes++;
      h[0]=h[1]; h[1]=h[2]; h[2]=h[3]; h[3]=b;
      if (h[0]==0xF4 && h[1]==0xF3 && h[2]==0xF2 && h[3]==0xF1) tramas++;
    }
  }
  // 2) mandar comando (3 veces) y buscar ACK (FD FC FB FA)
  int acks = 0;
  for (int k = 0; k < 3 && !acks; k++) {
    Serial1.write(CMD_ON, sizeof(CMD_ON));
    Serial1.flush();
    t0 = millis();
    while (millis() - t0 < 300) {
      while (Serial1.available()) {
        uint8_t b = Serial1.read();
        h[0]=h[1]; h[1]=h[2]; h[2]=h[3]; h[3]=b;
        if (h[0]==0xFD && h[1]==0xFC && h[2]==0xFB && h[3]==0xFA) acks++;
      }
    }
  }
  Serial1.write(CMD_OFF, sizeof(CMD_OFF));
  delay(100);
  Serial1.end();

  Serial.printf("  RX=GPIO%d TX=GPIO%d @%ld: %4d bytes, %3d tramas de datos, ACK al comando: %s\n",
                rx, tx, baud, bytes, tramas, acks ? "SI" : "no");
  if (tramas > 0) {
    if (rxBueno < 0 || acks) { rxBueno = rx; txBueno = tx; baudBueno = baud; }
  }
  return tramas > 0 && acks > 0;
}

void diagnostico() {
  Serial.println();
  Serial.println("========== DIAGNOSTICO LD2410C ==========");
  Serial.println("[1] Nivel electrico de cada pin (UART apagado):");
  for (int i = 0; i < NPINES; i++) revisarPin(PINES[i]);

  Serial.println("[2] Probando UART del radar:");
  rxBueno = -1;
  long bauds[] = {256000, 115200};
  int pares[][2] = {{5,6},{6,5},{3,5},{5,3},{3,6},{6,3}};
  for (long bd : bauds) {
    for (auto &pr : pares) {
      if (probarUart(pr[0], pr[1], bd)) goto fin;
    }
  }
fin:
  if (rxBueno >= 0) {
    Serial.printf("RESULTADO: radar TX -> GPIO%d, radar RX -> GPIO%d, %ld baudios\n", rxBueno, txBueno, baudBueno);
  } else {
    Serial.println("RESULTADO: NO llega ninguna trama del radar por 3, 5 ni 6.");
    Serial.println("           Revisar VCC del radar en 5V, GND comun, y TX del radar.");
  }
  Serial.println("=========================================");
}

void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 4000) delay(10);
  delay(1500);
  Serial.println("Diagnostico radar iniciado");
}

unsigned long ultimoDiag = 0;
void loop() {
  if (ultimoDiag == 0 || millis() - ultimoDiag > 30000) {
    diagnostico();
    ultimoDiag = millis();
    // Modo en vivo: solo RX, para poder leer GPIO3 y GPIO6 con pull-down
    if (rxBueno >= 0) Serial1.begin(baudBueno, SERIAL_8N1, rxBueno, -1);
    pinMode(4, INPUT_PULLDOWN);
    if (rxBueno != 6) pinMode(6, INPUT_PULLDOWN);
  }
  // Monitoreo en vivo: estado de pines y ultima trama
  static unsigned long tPrint = 0;
  static uint8_t buf[64]; static int n = 0;
  if (rxBueno >= 0) {
    while (Serial1.available()) {
      uint8_t b = Serial1.read();
      if (n < 64) buf[n++] = b; else n = 0;
      if (n >= 4 && buf[n-4]==0xF8 && buf[n-3]==0xF7 && buf[n-2]==0xF6 && buf[n-1]==0xF5) {
        // F4F3F2F1 len(2) 02 AA estado movDist(2) movE estDist(2) estE detDist(2) 55 00 F8F7F6F5
        if (n >= 23 && buf[0]==0xF4 && buf[6]==0x02 && buf[7]==0xAA && millis() - tPrint > 500) {
          int estado = buf[8];
          int movD = buf[9] | (buf[10] << 8);
          int estD = buf[12] | (buf[13] << 8);
          const char* txt[] = {"nadie", "movimiento", "quieto", "mov+quieto"};
          Serial.printf("radar: %-10s mov=%3dcm est=%3dcm | OUT(GPIO4)=%d\n",
                        estado < 4 ? txt[estado] : "?", movD, estD, digitalRead(4));
          tPrint = millis();
        }
        n = 0;
      }
      if (n == 4 && !(buf[0]==0xF4 && buf[1]==0xF3 && buf[2]==0xF2 && buf[3]==0xF1)) {
        memmove(buf, buf + 1, 3); n = 3;
      }
    }
  } else if (millis() - tPrint > 1000) {
    pinMode(3, INPUT);
    Serial.printf("sin UART | GPIO3=%d GPIO5=%d GPIO6=%d\n", digitalRead(3), digitalRead(5), digitalRead(6));
    tPrint = millis();
  }
}

# Sistema de vigilancia del antejardín

Proyecto de **Internet de las Cosas**, grupo 7788, Universidad Cooperativa de Colombia.
**Equipo:** Keyner Madrid, Luisa Alzate, Juan Vega y Paula Fernandez · **Docente:** Juan Osorio

Un sistema IoT que observa el antejardín de una casa y le dice al residente **si alguien se está acercando a la puerta**, no solo si "hay movimiento". Mide si hay alguien moviéndose y a qué distancia está, valida cada dato y decide: `TRANQUILO`, `MOVIMIENTO`, `ALERTA`, `RECHAZADO` o `SIN SEÑAL`.

📄 **Informe del Momento 3:** [`informe/Informe_Momento3_Vigilancia_Antejardin.pdf`](informe/Informe_Momento3_Vigilancia_Antejardin.pdf)

---

## Prototipo físico (Momento 3)

| Pieza | Función |
|---|---|
| ESP32-C3 SuperMini | Lee los dos sensores y envía el dato por USB |
| LiDAR **TF-Luna** | Distancia en cm (20 a 800 cm), unas 97 lecturas/s |
| Radar **HLK-LD2410C** | Movimiento / presencia quieta y la distancia del objetivo, 10 tramas/s |
| Computador | `servidor.py`: valida, decide cada 2 s, guarda el registro y sirve el panel |

### Recorrido del dato

```
TF-Luna ──UART 115200 (GPIO20/21)──┐
                                   ├─► ESP32-C3 ──USB serial (JSON cada 200 ms)──► servidor.py ──► panel en el navegador
Radar  ──UART 256000 (GPIO5/6)─────┘                                              (valida, decide,     (http://localhost:8765)
        + OUT (GPIO4)                                                              guarda CSV)
```

### Conexiones (confirmadas por diagnóstico)

| Sensor | Pin del sensor | ESP32-C3 SuperMini |
|---|---|---|
| Radar LD2410C | VCC / GND | 5V / GND (protoboard) |
| | TX | GPIO5 |
| | RX | GPIO6 |
| | OUT | GPIO4 |
| TF-Luna | +5V (pin 1) / GND (pin 4) | 5V / GND (protoboard) |
| | TXD (pin 3) | GPIO20 |
| | RXD (pin 2) | GPIO21 |

El pin de 3.3V no se usa. Los pines 5 y 6 del TF-Luna quedan sin conectar. No se usan GPIO2, GPIO8 ni GPIO9 (pines de arranque).

### Reglas de decisión

| Regla | Resultado |
|---|---|
| Distancia fuera de 20–800 cm, intensidad < 100 o = 65535, o sensor sin datos | `RECHAZADO`: el dato no se usa |
| El radar no ve movimiento | `TRANQUILO` |
| Movimiento sin acercamiento sostenido | `MOVIMIENTO` |
| Movimiento y la distancia baja 3 lecturas seguidas, al menos 30 cm, terminando a ≤ 300 cm | `ALERTA`: persona se aproxima |
| 7 s sin datos | `SIN SEÑAL`: no se asume que todo está tranquilo |

---

## Cómo ejecutarlo

**Requisitos:** VS Code con [PlatformIO](https://platformio.org/) y Python 3 con `pyserial` (`pip install pyserial`).

1. **Firmware:** abre la carpeta `firmware/` con PlatformIO y carga el programa al ESP32-C3 (`pio run -t upload`).
2. **Panel:** ejecuta
   ```bash
   python dashboard/servidor.py
   ```
   o haz doble clic en `dashboard/abrir_panel.bat`. Se abre en `http://localhost:8765`; la vista de los dos sensores juntos está en `http://localhost:8765/conjunto`. El panel encuentra solo el puerto del ESP32 y tiene un botón para compilar y cargar el firmware.

> Mientras el panel está abierto, el puerto serial está ocupado: ciérralo antes de usar `pio device monitor`.

### Avisos a Telegram (opcional)

El servidor avisa al celular solo lo importante:

| Decisión | Aviso |
|---|---|
| `ALERTA` | 🚨 Persona se aproxima (trayectoria en cm). Máximo uno por minuto |
| `SIN SEÑAL` | ⚠️ No llegan datos del sensor |
| Señal recuperada | ✅ El sistema volvió a recibir datos |
| `RECHAZADO` ≥ 80 % durante 30 s | ⚠️ Datos no válidos, revisar el sensor |

Para activarlo:
1. En Telegram, habla con **@BotFather**, envía `/newbot` y copia el token.
2. Copia `dashboard/telegram.ejemplo.json` como `dashboard/telegram.json` y pega el token. **Este archivo no se sube al repositorio.**
3. Abre tu bot, presiona **Iniciar** y escríbele "hola".
4. En el panel, en la tarjeta *Notificaciones Telegram*, presiona **Buscar mi chat_id** y luego **Enviar prueba**.

No hace falta instalar nada más: se usa la biblioteca estándar de Python.

---

## Estructura

```
firmware/                 Proyecto PlatformIO del ESP32-C3 (firmware "monitor-1")
  src/main.cpp            Lee radar y TF-Luna, revisa las tramas y envía JSON por USB
  diagnosticos/           Programas usados para comprobar el cableado de cada sensor
dashboard/                Panel local
  servidor.py             Lee el puerto serial, valida, decide, guarda, sirve el panel y avisa por Telegram
  telegram.ejemplo.json   Plantilla de configuración del bot (copiar como telegram.json)
  index.html              Panel: decisión, sensores, gráfica, cableado, registro y CSV
  conjunto.html           Vista "En conjunto" y guía de instalación
momento2/                 Entrega anterior (simulación en Wokwi, MQTT con HiveMQ)
  receptor.py             Receptor MQTT con validación y firma HMAC
  prueba_intruso.py       Prueba de seguridad: avisos falsos y reenviados
  simulacion_wokwi/       Firmware y circuito de la simulación (HC-SR04 + PIR)
informe/                  Informe del Momento 3, capturas y datos reales de la prueba (CSV)
```

## Qué cambió respecto al Momento 2

- Pasamos de la simulación en Wokwi a un **montaje físico** real.
- HC-SR04 → **TF-Luna** (rango 2–400 cm → **20–800 cm**, más una regla de intensidad); PIR → **radar LD2410C**.
- Sacamos el sensor de movimiento de GPIO2 (pin de arranque) y **comprobamos cada conexión por software**.
- Si un sensor deja de mandar datos, el ESP32 lo detecta y lo marca como caído (`ok:false`), y el servidor lo rechaza.
- **Panel local** en vivo con registro descargable en CSV.
- **Avisos al celular por Telegram**: en el Momento 2 la orden de avisar solo quedaba escrita en la consola.

## Limitaciones conocidas

- En el prototipo físico el dato viaja por **USB**, todavía no por WiFi/MQTT. La firma HMAC de `momento2/` protege ese camino MQTT, que aún no está activo en la placa.
- La clave HMAC de `momento2/` (`clave-secreta-ucc7788`) es **de demostración** y solo se usó en la simulación: hay que cambiarla y sacarla del código antes de cualquier uso real.
- No hay cámara. El panel no pide usuario (solo escucha en `127.0.0.1`).
- Los avisos de Telegram salen desde el computador: sin computador o sin internet no hay avisos. Telegram ve el texto de los avisos.
- Probado en mesa, no instalado. El límite de 300 cm es provisional.

Más detalle en el [informe](informe/Informe_Momento3_Vigilancia_Antejardin.pdf).

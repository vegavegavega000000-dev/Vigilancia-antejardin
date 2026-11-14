"""Panel local de vigilancia del antejardín.

Lee el ESP32-C3 por USB serial (firmware "monitor-1"), aplica las mismas reglas
que receptor.py (rango TF-Luna 20 a 800 cm) y sirve un dashboard en
http://localhost:8765. También permite repetir el chequeo de cableado y cargar
el firmware con PlatformIO sin cerrar nada a mano.

Además envía avisos a Telegram (ALERTA, SIN SEÑAL, señal recuperada y sensor
caído) si existe telegram.json con el token del bot. Ver telegram.ejemplo.json.

Uso:  python servidor.py            (abre el navegador)
      python servidor.py --sin-navegador
Requiere: pyserial  (pip install pyserial)
"""
import csv
import io
import json
import os
import queue
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial
import serial.tools.list_ports

# ---------------- Configuración ----------------
AQUI = os.path.dirname(os.path.abspath(__file__))
PROYECTO_PIO = os.path.join(os.path.dirname(AQUI), "firmware")  # carpeta del proyecto PlatformIO
PIO = os.path.join(os.environ.get("USERPROFILE", ""), ".platformio", "penv", "Scripts", "pio.exe")
PUERTO_HTTP = 8765
VID_ESP32 = 0x303A

# Reglas (las mismas del receptor, con el rango del TF-Luna)
RANGO_CM = (20, 800)
AMP_MIN, AMP_SATURADA = 100, 65535
TIMEOUT_S = 7          # sin datos en este tiempo -> SIN SEÑAL
ZONA_CM, BAJA_CM = 300, 30
PERIODO_DECISION_S = 2  # igual que la publicación MQTT prevista

ESTADOS_RADAR = {0: "nadie", 1: "movimiento", 2: "quieto", 3: "movimiento y quieto"}

# Telegram
TELEGRAM_ARCHIVO = os.path.join(AQUI, "telegram.json")   # no se sube al repositorio
ALERTA_PAUSA_S = 60        # como máximo un aviso de ALERTA por minuto
RECHAZO_AVISO_S = 30       # avisar si los datos se rechazan seguido durante 30 s

# ---------------- Estado compartido ----------------
cerrojo = threading.Lock()
estado = {
    "conectado": False,
    "puerto": None,
    "puerto_elegido": None,   # None = automático
    "quiere_conectar": True,
    "cargando": False,
    "ultimo_dato": None,
    "t_ultimo_dato": 0.0,
    "info": None,
    "chequeo": None,
    "decision": {"nivel": "SIN_SENAL", "texto": "Esperando datos del ESP32", "t": time.time()},
}
historial = deque(maxlen=20000)     # filas para el CSV
hist_decision = []
suscriptores = []
cola_escritura = queue.Queue()


def publicar(evento):
    texto = json.dumps(evento, ensure_ascii=False)
    with cerrojo:
        subs = list(suscriptores)
    for q in subs:
        try:
            q.put_nowait(texto)
        except queue.Full:
            pass


def log(texto, nivel="info"):
    publicar({"tipo": "log", "nivel": nivel, "texto": texto, "t": time.time()})


def resumen_estado():
    with cerrojo:
        return {
            "tipo": "estado",
            "conectado": estado["conectado"],
            "puerto": estado["puerto"],
            "puerto_elegido": estado["puerto_elegido"],
            "cargando": estado["cargando"],
            "info": estado["info"],
            "chequeo": estado["chequeo"],
            "decision": estado["decision"],
            "telegram": telegram_estado(),
        }


# ---------------- Telegram ----------------
tg = {"ultimo": None, "error": None, "enviados": 0}
cola_telegram = queue.Queue()
tg_regla = {"nivel_anterior": None, "t_ultima_alerta": 0.0, "ventana": deque(),
            "rechazo_avisado": False, "ultimo_motivo": ""}


def telegram_config():
    """Lee telegram.json en cada uso, así no hay que reiniciar al llenarlo."""
    try:
        with open(TELEGRAM_ARCHIVO, encoding="utf-8") as f:
            c = json.load(f)
        return str(c.get("token", "")).strip(), str(c.get("chat_id", "")).strip()
    except (OSError, ValueError):
        return "", ""


def telegram_estado():
    token, chat = telegram_config()
    return {"token": bool(token) and ":" in token, "chat_id": bool(chat),
            "ultimo": tg["ultimo"], "error": tg["error"], "enviados": tg["enviados"]}


def telegram_api(metodo, datos=None):
    token, _ = telegram_config()
    if not token:
        raise RuntimeError("falta el token en telegram.json")
    url = f"https://api.telegram.org/bot{token}/{metodo}"
    cuerpo = urllib.parse.urlencode(datos).encode() if datos else None
    try:
        with urllib.request.urlopen(url, data=cuerpo, timeout=10) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        try:
            desc = json.load(e).get("description", "")
        except Exception:
            desc = ""
        raise RuntimeError(f"Telegram respondió {e.code} {desc}".strip()) from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"sin conexión a Telegram ({e.reason})") from None


def avisar(tipo, texto):
    """Pone un aviso en la cola; lo envía el hilo de Telegram."""
    cola_telegram.put((tipo, texto, time.time()))


def hilo_telegram():
    while True:
        tipo, texto, t = cola_telegram.get()
        token, chat = telegram_config()
        if not token or not chat:
            continue  # sin configurar: no se envía nada
        hora = datetime.fromtimestamp(t).strftime("%H:%M:%S · %d/%m/%Y")
        mensaje = f"{texto}\n🕒 {hora}"
        try:
            telegram_api("sendMessage", {"chat_id": chat, "text": mensaje})
            tg["ultimo"] = {"tipo": tipo, "texto": texto.split("\n")[0], "t": t}
            tg["error"] = None
            tg["enviados"] += 1
            log(f"Telegram: aviso enviado ({tipo})")
        except Exception as e:
            tg["error"] = str(e)
            log(f"Telegram: no se pudo enviar el aviso ({tipo}): {e}", "alerta")
        publicar({"tipo": "telegram", **telegram_estado()})


def telegram_buscar_chat():
    """Busca el chat de quien le escribió al bot y lo guarda en telegram.json."""
    token, chat = telegram_config()
    if not token:
        return {"ok": False, "error": "Primero escribe el token del bot en telegram.json"}
    try:
        r = telegram_api("getUpdates")
    except Exception as e:
        return {"ok": False, "error": str(e)}
    chats = {}
    for u in r.get("result", []):
        m = u.get("message") or u.get("edited_message") or {}
        c = m.get("chat") or {}
        if "id" in c:
            chats[str(c["id"])] = c.get("first_name") or c.get("title") or ""
    if not chats:
        return {"ok": False, "error": "No encontré mensajes. Abre el bot en Telegram, presiona Iniciar "
                                      "(o escríbele 'hola') y vuelve a intentar."}
    elegido = list(chats)[-1]
    try:
        with open(TELEGRAM_ARCHIVO, encoding="utf-8") as f:
            cfg = json.load(f)
        cfg["chat_id"] = elegido
        with open(TELEGRAM_ARCHIVO, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception as e:
        return {"ok": False, "error": f"no se pudo guardar telegram.json: {e}"}
    publicar({"tipo": "telegram", **telegram_estado()})
    log(f"Telegram: chat encontrado y guardado ({chats[elegido] or 'sin nombre'})")
    return {"ok": True, "nombre": chats[elegido]}


def telegram_evaluar(nivel, texto, cargando):
    """Decide qué decisiones merecen un aviso al celular."""
    r = tg_regla
    ahora = time.time()
    anterior = r["nivel_anterior"]
    r["nivel_anterior"] = nivel

    if nivel == "ALERTA" and ahora - r["t_ultima_alerta"] >= ALERTA_PAUSA_S:
        r["t_ultima_alerta"] = ahora
        avisar("ALERTA", f"🚨 ALERTA · Antejardín\n{texto[0].upper() + texto[1:]}.\n"
                         "Revisa la cámara o sal a mirar.")
    if nivel == "SIN_SENAL" and anterior not in (None, "SIN_SENAL") and not cargando:
        avisar("SIN SEÑAL", "⚠️ SIN SEÑAL · Antejardín\nNo llegan datos del sensor hace más de "
                            f"{TIMEOUT_S} s.\nNo se asume que todo está tranquilo: revisa el dispositivo.")
    if anterior == "SIN_SENAL" and nivel != "SIN_SENAL":
        avisar("SEÑAL RECUPERADA", "✅ Señal recuperada · Antejardín\nEl sistema volvió a recibir datos "
                                   "y sigue vigilando.")
    # Datos rechazados de forma sostenida: >= 80 % de las decisiones de los últimos 30 s
    if nivel != "SIN_SENAL":
        r["ventana"].append((ahora, nivel == "RECHAZADO"))
        if nivel == "RECHAZADO":
            r["ultimo_motivo"] = texto
    while r["ventana"] and r["ventana"][0][0] < ahora - RECHAZO_AVISO_S:
        r["ventana"].popleft()
    lleno = r["ventana"] and ahora - r["ventana"][0][0] >= RECHAZO_AVISO_S - PERIODO_DECISION_S - 0.5
    proporcion = sum(x for _, x in r["ventana"]) / len(r["ventana"]) if r["ventana"] else 0
    if lleno and proporcion >= 0.8 and not r["rechazo_avisado"]:
        r["rechazo_avisado"] = True
        avisar("DATOS RECHAZADOS", f"⚠️ Datos no válidos · Antejardín\nEn los últimos {RECHAZO_AVISO_S} s casi todos "
                                   f"los datos se rechazaron ({r['ultimo_motivo']}).\n"
                                   "El sistema no decide con esos datos: revisa el sensor.")
    elif r["rechazo_avisado"] and lleno and proporcion <= 0.2:
        r["rechazo_avisado"] = False


# ---------------- Validación y decisión ----------------
def validar_luna(l):
    """None si la distancia sirve; si no, la razón."""
    if not l or not l.get("ok"):
        return "el TF-Luna no está mandando datos"
    d, a = l.get("dist"), l.get("amp")
    if a == AMP_SATURADA:
        return f"intensidad saturada ({a})"
    if a is None or a < AMP_MIN:
        return f"intensidad baja ({a}), lectura poco confiable"
    if not (RANGO_CM[0] <= d <= RANGO_CM[1]):
        return f"distancia {d} cm fuera del rango del sensor ({RANGO_CM[0]} a {RANGO_CM[1]} cm)"
    return None


def decidir(dato):
    r, l = dato.get("radar") or {}, dato.get("luna") or {}
    if not r.get("ok"):
        return "RECHAZADO", "el radar no está mandando datos: no se usa para decidir"
    razon = validar_luna(l)
    movimiento = r.get("estado") in (1, 3)
    if not movimiento:
        hist_decision.clear()
        if r.get("estado") == 2:
            return "TRANQUILO", "presencia quieta, sin movimiento"
        return "TRANQUILO", "sin movimiento"
    if razon:
        hist_decision.clear()
        return "RECHAZADO", f"hay movimiento pero la distancia no es válida ({razon})"
    d = l["dist"]
    hist_decision.append(d)
    del hist_decision[:-3]
    h = hist_decision
    if len(h) == 3 and h[0] > h[1] > h[2] and h[0] - h[2] >= BAJA_CM and d <= ZONA_CM:
        return "ALERTA", f"persona se aproxima ({h[0]} → {d} cm)"
    return "MOVIMIENTO", f"movimiento sin aproximación sostenida ({d} cm)"


def hilo_decision():
    ultimo_nivel = None
    while True:
        time.sleep(PERIODO_DECISION_S)
        with cerrojo:
            dato, t = estado["ultimo_dato"], estado["t_ultimo_dato"]
            cargando = estado["cargando"]
        if cargando:
            nivel, texto = "SIN_SENAL", "cargando firmware"
        elif not dato or time.time() - t > TIMEOUT_S:
            nivel, texto = "SIN_SENAL", f"no llegan datos hace más de {TIMEOUT_S} s. NO se asume que todo está tranquilo"
        else:
            nivel, texto = decidir(dato)
        dec = {"nivel": nivel, "texto": texto, "t": time.time()}
        with cerrojo:
            estado["decision"] = dec
        publicar({"tipo": "decision", **dec})
        try:
            telegram_evaluar(nivel, texto, cargando)
        except Exception as e:
            log(f"Telegram: error evaluando el aviso: {e}", "alerta")
        if nivel != ultimo_nivel:
            log(f"{nivel}: {texto}", "alerta" if nivel in ("ALERTA", "SIN_SENAL", "RECHAZADO") else "info")
            ultimo_nivel = nivel


# ---------------- Serial ----------------
def buscar_puerto():
    with cerrojo:
        elegido = estado["puerto_elegido"]
    if elegido:
        return elegido
    for p in serial.tools.list_ports.comports():
        if p.vid == VID_ESP32:
            return p.device
    return None


def procesar_linea(texto):
    if not texto.startswith("{"):
        publicar({"tipo": "crudo", "texto": texto})
        return
    try:
        m = json.loads(texto)
    except json.JSONDecodeError:
        publicar({"tipo": "crudo", "texto": texto})
        return
    tipo = m.get("tipo")
    if tipo == "dato":
        ahora = time.time()
        with cerrojo:
            estado["ultimo_dato"], estado["t_ultimo_dato"] = m, ahora
            nivel = estado["decision"]["nivel"]
        r, l = m.get("radar", {}), m.get("luna", {})
        razon = validar_luna(l)
        m["luna"]["valida"] = razon is None
        m["luna"]["razon"] = razon
        m["radar"]["texto"] = ESTADOS_RADAR.get(r.get("estado"), "?")
        m["t"] = ahora
        historial.append([
            datetime.fromtimestamp(ahora).isoformat(timespec="milliseconds"),
            r.get("ok"), r.get("estado"), r.get("movD"), r.get("movE"), r.get("estD"), r.get("estE"),
            r.get("detD"), r.get("out"), l.get("ok"), l.get("dist"), l.get("amp"), l.get("temp"),
            razon is None, nivel,
        ])
        publicar(m)
    elif tipo in ("info", "chequeo", "inicio"):
        with cerrojo:
            if tipo == "info":
                estado["info"] = m
            elif tipo == "chequeo":
                estado["chequeo"] = m
        if tipo == "inicio":
            log(f"El ESP32 arrancó (firmware {m.get('fw')}, motivo del reinicio: {m.get('reinicio')})",
                "alerta" if "brownout" in str(m.get("reinicio")) or "panic" in str(m.get("reinicio")) else "info")
        if tipo == "chequeo":
            log(f"Chequeo: radar responde={'sí' if m.get('radarAck') else 'NO'}, "
                f"TF-Luna versión={m.get('lunaVersion') or 'sin respuesta'}",
                "info" if m.get("radarAck") and m.get("lunaVersion") else "alerta")
        publicar(m)
        publicar(resumen_estado())


def hilo_serial():
    while True:
        with cerrojo:
            quiere = estado["quiere_conectar"] and not estado["cargando"]
        if not quiere:
            time.sleep(0.3)
            continue
        puerto = buscar_puerto()
        if not puerto:
            time.sleep(1)
            continue
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = puerto, 115200, 0.2
            s.dtr = False
            s.rts = False   # no reiniciar la placa al abrir
            s.open()
        except Exception as e:
            log(f"No se pudo abrir {puerto}: {e}", "alerta")
            time.sleep(2)
            continue
        with cerrojo:
            estado["conectado"], estado["puerto"] = True, puerto
        log(f"Conectado a {puerto}")
        publicar(resumen_estado())
        cola_escritura.put("info")
        resto = b""
        try:
            while True:
                with cerrojo:
                    seguir = estado["quiere_conectar"] and not estado["cargando"]
                if not seguir:
                    break
                while not cola_escritura.empty():
                    s.write((cola_escritura.get() + "\n").encode())
                datos = s.read(4096)
                if not datos:
                    continue
                resto += datos
                *lineas, resto = resto.split(b"\n")
                for ln in lineas:
                    t = ln.decode("utf-8", "replace").strip()
                    if t:
                        procesar_linea(t)
        except Exception as e:
            log(f"Se perdió la conexión con {puerto} ({type(e).__name__}). "
                "Si pasa seguido, revisar 5V/GND o el cable USB.", "alerta")
        finally:
            try:
                s.close()
            except Exception:
                pass
            with cerrojo:
                estado["conectado"] = False
            publicar(resumen_estado())
            time.sleep(1)


# ---------------- Carga de firmware ----------------
def cargar_firmware():
    with cerrojo:
        if estado["cargando"]:
            return False
        estado["cargando"] = True
        puerto = estado["puerto"] or buscar_puerto()
    publicar(resumen_estado())
    time.sleep(0.8)  # que el hilo serial suelte el puerto
    ok = False
    try:
        cmd = [PIO, "run", "-t", "upload"] + (["--upload-port", puerto] if puerto else [])
        publicar({"tipo": "carga", "linea": "$ " + " ".join(cmd)})
        p = subprocess.Popen(cmd, cwd=PROYECTO_PIO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace",
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        for linea in p.stdout:
            publicar({"tipo": "carga", "linea": linea.rstrip()})
        ok = p.wait() == 0
    except Exception as e:
        publicar({"tipo": "carga", "linea": f"Error: {e}"})
    finally:
        with cerrojo:
            estado["cargando"] = False
        publicar({"tipo": "carga_fin", "ok": ok})
        log("Firmware cargado" if ok else "La carga del firmware falló (ver salida)", "info" if ok else "alerta")
        publicar(resumen_estado())
    return True


# ---------------- HTTP ----------------
class Manejador(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj, codigo=200):
        cuerpo = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(cuerpo)))
        self.end_headers()
        self.wfile.write(cuerpo)

    def _cuerpo(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n))
        except Exception:
            return {}

    def do_GET(self):
        ruta = self.path.split("?")[0]
        paginas = {"/": "index.html", "/index.html": "index.html",
                   "/conjunto": "conjunto.html", "/conjunto.html": "conjunto.html"}
        if ruta in paginas:
            with open(os.path.join(AQUI, paginas[ruta]), "rb") as f:
                cuerpo = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(cuerpo)))
            self.end_headers()
            self.wfile.write(cuerpo)
        elif ruta == "/api/estado":
            self._json(resumen_estado())
        elif ruta == "/api/puertos":
            self._json([{"puerto": p.device, "descripcion": p.description, "esp32": p.vid == VID_ESP32}
                        for p in serial.tools.list_ports.comports()])
        elif ruta == "/api/csv":
            buf = io.StringIO()
            w = csv.writer(buf)
            w.writerow(["momento", "radar_ok", "radar_estado", "mov_cm", "mov_energia", "quieto_cm",
                        "quieto_energia", "deteccion_cm", "out", "luna_ok", "distancia_cm", "intensidad",
                        "temp_c", "distancia_valida", "decision"])
            w.writerows(list(historial))
            cuerpo = buf.getvalue().encode("utf-8-sig")
            nombre = datetime.now().strftime("vigilancia_%Y%m%d_%H%M%S.csv")
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Disposition", f'attachment; filename="{nombre}"')
            self.send_header("Content-Length", str(len(cuerpo)))
            self.end_headers()
            self.wfile.write(cuerpo)
        elif ruta == "/api/eventos":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = queue.Queue(maxsize=500)
            with cerrojo:
                suscriptores.append(q)
            try:
                self.wfile.write(f"data: {json.dumps(resumen_estado(), ensure_ascii=False)}\n\n".encode())
                self.wfile.flush()
                while True:
                    try:
                        texto = q.get(timeout=15)
                        self.wfile.write(f"data: {texto}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": sigo aqui\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass
            finally:
                with cerrojo:
                    if q in suscriptores:
                        suscriptores.remove(q)
        else:
            self.send_error(404)

    def do_POST(self):
        ruta = self.path.split("?")[0]
        b = self._cuerpo()
        if ruta == "/api/conectar":
            with cerrojo:
                estado["puerto_elegido"] = b.get("puerto") or None
                estado["quiere_conectar"] = True
            self._json({"ok": True})
        elif ruta == "/api/desconectar":
            with cerrojo:
                estado["quiere_conectar"] = False
            log("Desconectado a pedido del usuario")
            self._json({"ok": True})
        elif ruta == "/api/comando":
            cmd = str(b.get("comando", "")).strip()
            if cmd not in ("chequeo", "info"):
                self._json({"ok": False, "error": "comando no permitido"}, 400)
                return
            cola_escritura.put(cmd)
            log(f"Comando enviado al ESP32: {cmd}")
            self._json({"ok": True})
        elif ruta == "/api/cargar":
            iniciado = not estado["cargando"]
            if iniciado:
                threading.Thread(target=cargar_firmware, daemon=True).start()
            self._json({"ok": iniciado})
        elif ruta == "/api/telegram/prueba":
            token, chat = telegram_config()
            if not token or not chat:
                self._json({"ok": False, "error": "Falta el token o el chat_id en telegram.json"})
                return
            avisar("PRUEBA", "🔔 Prueba · Antejardín\nLas notificaciones del sistema de vigilancia están activas.")
            self._json({"ok": True})
        elif ruta == "/api/telegram/chatid":
            self._json(telegram_buscar_chat())
        else:
            self.send_error(404)


def main():
    threading.Thread(target=hilo_serial, daemon=True).start()
    threading.Thread(target=hilo_decision, daemon=True).start()
    threading.Thread(target=hilo_telegram, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", PUERTO_HTTP), Manejador)
    srv.daemon_threads = True
    url = f"http://localhost:{PUERTO_HTTP}"
    print(f"Panel de vigilancia en {url}  (Ctrl+C para salir)")
    if "--sin-navegador" not in sys.argv:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

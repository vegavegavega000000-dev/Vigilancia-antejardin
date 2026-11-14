import json, time, threading
from datetime import datetime
import paho.mqtt.client as mqtt
import hmac, hashlib 

HOST, PORT = "broker.hivemq.com", 1883
TOPIC = "ucc7788/casa/antejardin/aviso"
TOPIC_ESTADO = "ucc7788/casa/antejardin/estado"
TIMEOUT_S = 7          # sin mensajes en este tiempo -> SIN SEÑAL
ZONA_CM, BAJA_CM = 300, 30

CAMPOS = ["id", "movimiento", "distancia", "unidad", "momento", "version"]
ultimo = time.time()
sin_senal = False
hist = []

CLAVE = b"clave-secreta-ucc7788"
ultimo_momento = None

def firma_ok(m):
    canon = (f'{m["id"]}|{"true" if m["movimiento"] else "false"}|'
             f'{m["distancia"]:.1f}|{m["unidad"]}|{m["momento"]}|{m["version"]}')
    esperada = hmac.new(CLAVE, canon.encode(), hashlib.sha256).hexdigest()
    return isinstance(m.get("firma"), str) and hmac.compare_digest(esperada, m["firma"])

def validar(m):
    """Devuelve None si el mensaje es válido, o la razón del rechazo."""
    if not isinstance(m, dict):
        return "no es un objeto JSON"
    for c in CAMPOS:
        if c not in m:
            return f"falta el campo '{c}'"
        if m[c] is None:
            return f"el campo '{c}' viene vacío (sin dato no es cero)"
    if not isinstance(m["movimiento"], bool):
        return "movimiento debe ser true/false"
    d = m["distancia"]
    if isinstance(d, bool) or not isinstance(d, (int, float)):
        return "distancia no es un número"
    if m["unidad"] != "cm":
        return f"unidad '{m['unidad']}' no es cm"
    if m["version"] != 1:
        return f"versión {m['version']} no soportada"
    if not (2 <= d <= 400):
        return f"distancia {d} cm es imposible (el sensor mide 2 a 400 cm)"
    try:
        datetime.fromisoformat(m["momento"])
    except Exception:
        return "momento no es una fecha válida"
    return None

def decidir(m):
    d = m["distancia"]
    if not m["movimiento"]:
        hist.clear()
        return "TRANQUILO: sin movimiento"
    hist.append(d)
    del hist[:-3]
    if (len(hist) == 3 and hist[0] > hist[1] > hist[2]
            and hist[0] - hist[2] >= BAJA_CM and d <= ZONA_CM):
        return f"ALERTA: persona se aproxima ({hist[0]} -> {d} cm). Iniciar grabación y notificar"
    return f"MOVIMIENTO sin aproximación sostenida ({d} cm): alguien pasando por el andén"

def on_connect(c, u, f, rc, p=None):
    c.subscribe([(TOPIC, 0), (TOPIC_ESTADO, 0)])
    print("Receptor conectado, esperando avisos...")

def on_message(c, u, msg):
    global ultimo, sin_senal, ultimo_momento
    texto = msg.payload.decode(errors="replace")
    if msg.topic == TOPIC_ESTADO:
        print(f"[ESTADO DISPOSITIVO] {texto}")
        return
    try:
        m = json.loads(texto)
    except Exception:
        print("[RECHAZADO] no es JSON válido")
        return
    razon = validar(m)
    if razon is None and not firma_ok(m):
        razon = "firma ausente o inválida: posible aviso falso"
    if razon is None:
        t = datetime.fromisoformat(m["momento"])
        if t.tzinfo is None:
            razon = "momento sin zona horaria"
        elif abs((datetime.now(t.tzinfo) - t).total_seconds()) > 30:
            razon = "desfase mayor a 30 s"
        elif ultimo_momento and t <= ultimo_momento:
            razon = "momento repetido o anterior al último aviso: posible reenvío"
    if razon:
        print(f"[RECHAZADO] {razon} -> no se usa para decidir | recibido: {texto}")
        return
    ultimo_momento = t
    ultimo = time.time()
    if sin_senal:
        sin_senal = False
        print("[SEÑAL RECUPERADA]")
    print(f"[OK] {decidir(m)} | recibido: {texto}")

def vigilante():
    global sin_senal
    while True:
        time.sleep(1)
        if time.time() - ultimo > TIMEOUT_S and not sin_senal:
            sin_senal = True
            print(f"[SIN SEÑAL] no llegan datos hace más de {TIMEOUT_S} s. "
                  "NO se asume que todo está tranquilo.")

cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
cli.on_connect, cli.on_message = on_connect, on_message
cli.connect(HOST, PORT)
threading.Thread(target=vigilante, daemon=True).start()
cli.loop_forever()
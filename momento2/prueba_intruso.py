import json, time
from datetime import datetime, timezone, timedelta
import paho.mqtt.client as mqtt

TOPIC = "ucc7788/casa/antejardin/aviso"
capturado = []

def on_message(c, u, msg):
    m = json.loads(msg.payload)
    f = m.get("firma")
    if f and f != "0" * 64 and "distancia" in m and not capturado:
        capturado.append(msg.payload)

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.on_message = on_message
c.connect("broker.hivemq.com", 1883)
c.subscribe(TOPIC)
c.loop_start()

ahora = datetime.now(timezone(timedelta(hours=-5))).strftime("%Y-%m-%dT%H:%M:%S-05:00")
base = {"id": "antejardin-n1", "movimiento": True, "distancia": 120.0,
        "unidad": "cm", "momento": ahora, "version": 1}

print("1) aviso falso SIN firma");           c.publish(TOPIC, json.dumps(base)); time.sleep(2)
print("2) aviso falso con firma inventada"); c.publish(TOPIC, json.dumps(dict(base, firma="0" * 64))); time.sleep(2)
print("3) reenvío de un aviso legítimo capturado")
while not capturado: time.sleep(0.5)
c.publish(TOPIC, capturado[0]); time.sleep(2)
c.loop_stop()
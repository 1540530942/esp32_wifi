#!/usr/bin/env python3
"""Clear command messages left retained on the broker.

server.py used to publish each command with retain=True on a per-command topic.
Every one that was not cleared is still sitting on the broker, and the device
subscribes with a wildcard, so every reconnect replays the whole backlog. That
produced bursts of a dozen stale stop_audio commands killing live playback --
which looked exactly like barge-in misfires and cost four firmware versions of
VAD tuning before anyone read the acknowledgements.

Publishing retain=False fixes new commands. It does nothing about the ones
already on the broker: a retained message is removed only by publishing an
empty payload to the same topic. This does that for every command the hub has a
record of.

Run on the host where the broker lives:
    python3 purge_retained_commands.py [--dry-run]
"""
import argparse
import json
import os
import secrets
import sys
import time
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt not available; run this where device_hub runs")

DATA_FILE = Path(os.environ.get("DEVICE_HUB_DATA", "/data/devices.json"))
MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not DATA_FILE.exists():
        sys.exit(f"no device data at {DATA_FILE}; set DEVICE_HUB_DATA")
    data = json.loads(DATA_FILE.read_text())

    topics = []
    for device_id, rec in data.items():
        for command in rec.get("commands", []):
            cid = command.get("id")
            if cid:
                topics.append((f"devices/{device_id}/command/{cid}",
                               command.get("action"), command.get("status")))

    print(f"{len(topics)} command topics on record")
    stale = [t for t in topics if t[2] != "done"]
    print(f"  of which {len(stale)} are not done -- those are the ones that "
          f"replay and re-execute")
    for topic, action, status in stale[:10]:
        print(f"    {action} ({status}) {topic}")
    if len(stale) > 10:
        print(f"    ... and {len(stale) - 10} more")

    if args.dry_run:
        print("dry run; nothing published")
        return 0

    # Clear every topic, not just the stale ones: a command marked done can
    # still be retained if the clear failed at the time, and re-executing a
    # completed command is exactly the failure being removed.
    client = mqtt.Client(client_id=f"purge-{secrets.token_hex(4)}",
                         protocol=mqtt.MQTTv311)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    cleared = 0
    for topic, _, _ in topics:
        info = client.publish(topic, payload=b"", qos=1, retain=True)
        info.wait_for_publish(timeout=5)
        if info.rc == mqtt.MQTT_ERR_SUCCESS:
            cleared += 1
    time.sleep(1)
    client.loop_stop()
    client.disconnect()
    print(f"cleared {cleared}/{len(topics)} retained command topics")
    return 0 if cleared == len(topics) else 1


if __name__ == "__main__":
    sys.exit(main())

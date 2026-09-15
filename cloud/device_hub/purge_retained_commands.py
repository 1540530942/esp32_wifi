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


def _broker_retained_topics(settle_s: float = 6.0) -> list[str]:
    """Every retained command topic the broker will replay to a subscriber.

    Subscribing to the wildcard delivers exactly what a reconnecting device
    gets, which is the thing that actually matters -- there is no API to list
    retained messages, so the only honest way to enumerate them is to receive
    them the same way the device does.
    """
    seen: list[str] = []
    client = mqtt.Client(client_id=f"scan-{secrets.token_hex(4)}",
                         protocol=mqtt.MQTTv311)

    def on_message(_c, _u, msg):
        # A zero-length payload IS the tombstone that clears a retained topic,
        # so it is not a live command and must not be counted as one.
        if msg.payload and msg.topic not in seen:
            seen.append(msg.topic)

    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.subscribe("devices/+/command/#", qos=1)
    client.loop_start()
    time.sleep(settle_s)
    client.loop_stop()
    client.disconnect()
    return seen


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

    # Also ask the broker what it is actually holding. devices.json is the
    # hub's record, not the broker's, and the two diverge: the command list is
    # trimmed, and a hub redeploy or a restored data file loses ids whose
    # retained message is still sitting on the broker and still replaying.
    #
    # This matters -- clearing only what devices.json remembered left a day's
    # worth of retained play_audio on the broker. Every reconnect delivered the
    # whole backlog at once, each play_audio stopping the one before it, which
    # surfaced as "wav_playback ESP_ERR_INVALID_STATE" with played=0 and looked
    # like a broken player. A reboot made it worse rather than better, because a
    # reboot is a reconnect.
    known = {t for t, _, _ in topics}
    for topic in _broker_retained_topics():
        if topic not in known:
            topics.append((topic, "?", "retained-on-broker"))

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

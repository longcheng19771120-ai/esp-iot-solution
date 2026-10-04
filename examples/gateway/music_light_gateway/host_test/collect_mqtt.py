#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""
Collect labelled feature rows from a running gateway for fit_mood_model.py.

This calibrates with the gateway's own microphone in the real room, which is
what the model will hear in use. Play one song, tag it, repeat:

    pip install paho-mqtt
    python3 collect_mqtt.py --broker broker.emqx.io --id <gateway_id> -o room.csv
    # in another terminal, for each song (send again for every new song):
    mosquitto_pub -h broker.emqx.io -t 'music-light/<gateway_id>/cmd' -m 'label happy'
    # quadrants: calm, happy, tense, sad; or exact values: 'label 0.3 0.9' (valence energy)
    ...
    mosquitto_pub -h broker.emqx.io -t 'music-light/<gateway_id>/cmd' -m 'label none'

Every analysis window (5 s) while a label is set becomes one CSV row.
"""

import argparse
import json

import paho.mqtt.client as mqtt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--broker', default='broker.emqx.io')
    ap.add_argument('--port', type=int, default=1883)
    ap.add_argument('--prefix', default='music-light', help='CONFIG_GATEWAY_MQTT_TOPIC_PREFIX')
    ap.add_argument('--id', required=True, help='gateway id printed in the device log')
    ap.add_argument('-o', '--output', required=True, help='CSV file to append to')
    args = ap.parse_args()

    topic = '{}/{}/music'.format(args.prefix, args.id)
    counts = {}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        client.subscribe(topic)
        print('Listening on', topic)

    def on_message(client, userdata, msg):
        try:
            m = json.loads(msg.payload)
        except ValueError:
            return
        label = m.get('label')
        # Silent windows carry no mood information
        if not label or m.get('silent', 0) > 0.7:
            return
        source = 'device-session-{}'.format(m.get('session', 0))
        with open(args.output, 'a') as f:
            f.write(','.join(['{:.2f}'.format(label[0]), '{:.2f}'.format(label[1]), source,
                              '{:.3f}'.format(m['valence']), '{:.3f}'.format(m['energy'])] +
                             ['{:.4f}'.format(x) for x in m['vec']]) + '\n')
        counts[source] = counts.get(source, 0) + 1
        print('{:<20} label v {:.2f} e {:.2f} | estimate v {:.2f} e {:.2f} | windows {}'.format(
            source, label[0], label[1], m.get('valence', 0), m.get('energy', 0), counts[source]))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port)
    client.loop_forever()


if __name__ == '__main__':
    main()

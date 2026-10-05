#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""
Collect labelled feature rows from a running gateway for fit_genre_model.py
and fit_mood_model.py.

This calibrates with the gateway's own microphone in the real room, which is
what the models will hear in use. Play one song, tag it, repeat:

    pip install paho-mqtt
    python3 collect_mqtt.py --broker broker.emqx.io --id <gateway_id> -g genre.csv -m mood.csv
    # in another terminal, for each song (send again for every new song):
    mosquitto_pub -h broker.emqx.io -t 'music-light/<gateway_id>/cmd' -m 'label rock happy'
    # a genre (ambient, classical, pop, rock, electronic, hiphop), a mood (calm, happy,
    # tense, sad, or 'valence energy' such as '0.3 0.9'), or both
    ...
    mosquitto_pub -h broker.emqx.io -t 'music-light/<gateway_id>/cmd' -m 'label none'

Every analysis window (5 s) while a label is set becomes one CSV row: genre
labels go to the -g file, mood labels to the -m file.
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
    ap.add_argument('-g', '--genre-output', help='CSV file for genre rows (fit_genre_model.py)')
    ap.add_argument('-m', '--mood-output', help='CSV file for mood rows (fit_mood_model.py)')
    args = ap.parse_args()
    if not args.genre_output and not args.mood_output:
        ap.error('give -g, -m or both')

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
        # Silent windows carry no genre or mood information
        if m.get('silent', 0) > 0.7:
            return
        source = 'device-session-{}'.format(m.get('session', 0))
        genre, label = m.get('genre_label'), m.get('label')
        if genre and args.genre_output:
            with open(args.genre_output, 'a') as f:
                f.write(','.join([genre, source] + ['{:.4f}'.format(x) for x in m['gvec']]) + '\n')
        if label and args.mood_output:
            with open(args.mood_output, 'a') as f:
                f.write(','.join(['{:.2f}'.format(label[0]), '{:.2f}'.format(label[1]), source,
                                  '{:.3f}'.format(m['valence']), '{:.3f}'.format(m['energy'])] +
                                 ['{:.4f}'.format(x) for x in m['vec']]) + '\n')
        if not genre and not label:
            return
        counts[source] = counts.get(source, 0) + 1
        print('{:<20} label {:<10} {:<11} | estimate {:<10} v {:.2f} e {:.2f} | windows {}'.format(
            source, genre or '-', '{:.2f} {:.2f}'.format(*label) if label else '-', m.get('genre', ''),
            m.get('valence', 0), m.get('energy', 0), counts[source]))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port)
    client.loop_forever()


if __name__ == '__main__':
    main()

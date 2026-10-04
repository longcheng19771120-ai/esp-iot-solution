#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""
Fit the on-device mood model from labelled feature rows.

Rows come from `analyze_wav --csv <label> file.wav` or from collect_mqtt.py
(recorded by the gateway's own microphone, which is the better calibration).
Each row is: valence,energy,source,current_valence,current_energy,v0..v10:
the labelled targets in 0..1, the song or recording it came from, what the
firmware estimated at the time, and the vector from mood_feature_vector().

    python3 fit_mood_model.py data/*.csv                    # leave-one-source-out report
    python3 fit_mood_model.py data/*.csv -o ../main/mood_model.h

The model is two L2-regularised logistic regressions on standardised
features. Only the Python standard library is needed.
"""

import argparse
import csv
import math
import sys
from collections import defaultdict

FEATURES = ['tempo', 'regularity', 'contrast', 'onset_rate', 'bass', 'mid', 'high', 'range', 'centroid',
            'mode', 'key_strength']
L2 = 0.05
STEPS = 2000
RATE = 0.5


def load(paths):
    rows = []
    for path in paths:
        with open(path, newline='') as f:
            for r in csv.reader(f):
                if not r or r[0].startswith('#'):
                    continue
                if len(r) != 5 + len(FEATURES):
                    sys.exit('{}: expected {} columns, got {}'.format(path, 5 + len(FEATURES), len(r)))
                v, e = float(r[0]), float(r[1])
                if not (0 <= v <= 1 and 0 <= e <= 1):
                    sys.exit('{}: valence and energy must be in 0..1'.format(path))
                rows.append((v, e, r[2], [float(x) for x in r[5:]], (float(r[3]), float(r[4]))))
    return rows


def standardise(rows):
    n = len(rows)
    mean = [sum(r[3][i] for r in rows) / n for i in range(len(FEATURES))]
    std = [math.sqrt(sum((r[3][i] - mean[i]) ** 2 for r in rows) / n) or 1.0 for i in range(len(FEATURES))]
    return mean, std


def sigmoid(z):
    return 1 / (1 + math.exp(-max(-30, min(30, z))))


def fit_logistic(xs, ys):
    """Gradient descent on cross-entropy with soft targets."""
    w = [0.0] * len(FEATURES)
    b = 0.0
    n = len(xs)
    for _ in range(STEPS):
        gw = [L2 * wi for wi in w]
        gb = 0.0
        for x, y in zip(xs, ys):
            err = sigmoid(b + sum(wi * xi for wi, xi in zip(w, x))) - y
            gb += err / n
            for i, xi in enumerate(x):
                gw[i] += err * xi / n
        w = [wi - RATE * g for wi, g in zip(w, gw)]
        b -= RATE * gb
    return w, b


def fit(rows):
    mean, std = standardise(rows)
    xs = [[(x - m) / s for x, m, s in zip(r[3], mean, std)] for r in rows]
    wv, bv = fit_logistic(xs, [r[0] for r in rows])
    we, be = fit_logistic(xs, [r[1] for r in rows])
    return {'mean': mean, 'std': std, 'wv': wv, 'bv': bv, 'we': we, 'be': be}


def predict(model, vec):
    x = [(v - m) / s for v, m, s in zip(vec, model['mean'], model['std'])]
    return (sigmoid(model['bv'] + sum(w * xi for w, xi in zip(model['wv'], x))),
            sigmoid(model['be'] + sum(w * xi for w, xi in zip(model['we'], x))))


def score(pairs):
    """pairs of (label, prediction): mean abs errors and share of windows in the right quadrant."""
    n = len(pairs)
    ev = sum(abs(p[0] - t[0]) for t, p in pairs) / n
    ee = sum(abs(p[1] - t[1]) for t, p in pairs) / n
    q = sum((p[0] >= 0.5) == (t[0] >= 0.5) and (p[1] >= 0.5) == (t[1] >= 0.5) for t, p in pairs) / n
    return ev, ee, q


def evaluate(rows):
    """Leave one source (song or recording) out, so a song never tests itself."""
    sources = sorted({r[2] for r in rows})
    fitted, current, per_source = [], [], []
    for src in sources:
        model = fit([r for r in rows if r[2] != src])
        test = [r for r in rows if r[2] == src]
        preds = [predict(model, r[3]) for r in test]
        fitted += [((r[0], r[1]), p) for r, p in zip(test, preds)]
        current += [((r[0], r[1]), r[4]) for r in test]
        per_source.append((src, test[0][0], test[0][1],
                           sum(p[0] for p in preds) / len(preds), sum(p[1] for p in preds) / len(preds),
                           sum(r[4][0] for r in test) / len(test), sum(r[4][1] for r in test) / len(test)))
    print('Leave-one-source-out over {} sources, {} windows:'.format(len(sources), len(rows)))
    print('  {:<10} {:>14} {:>14} {:>17}'.format('', 'valence error', 'energy error', 'quadrant correct'))
    for name, pairs in (('fitted', fitted), ('current', current)):
        ev, ee, q = score(pairs)
        print('  {:<10} {:>14.2f} {:>14.2f} {:>17.0%}'.format(name, ev, ee, q))
    print('  ("current" is what the firmware estimated while recording)')
    print('  {:<30} {:>11} {:>11} {:>11}'.format('source', 'label v/e', 'fitted v/e', 'current v/e'))
    for src, v, e, fv, fe, cv, ce in per_source:
        print('  {:<30} {:>5.2f}/{:<5.2f} {:>5.2f}/{:<5.2f} {:>5.2f}/{:<5.2f}'.format(src[-30:], v, e, fv, fe, cv, ce))
    quadrants = defaultdict(set)
    for r in rows:
        quadrants[(r[0] >= 0.5, r[1] >= 0.5)].add(r[2])
    if len(quadrants) < 4 or min(len(s) for s in quadrants.values()) < 3:
        print('  Note: fewer than 3 songs in some quadrant; the fit will not generalise yet.')


def write_header(model, path):
    def arr(values):
        return '{' + ', '.join('{:.5f}f'.format(x) for x in values) + '}'

    lines = [
        '/*',
        ' * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD',
        ' *',
        ' * SPDX-License-Identifier: Apache-2.0',
        ' */',
        '',
        '/* Generated by host_test/fit_mood_model.py, do not edit. */',
        '',
        '#pragma once',
        '',
        '#define MOOD_MODEL_TRAINED 1',
        '',
        '_Static_assert(MOOD_FEATURE_COUNT == {}, "regenerate mood_model.h");'.format(len(FEATURES)),
        '',
        '/* Features: ' + ', '.join(FEATURES) + ' */',
        'static const float mood_model_mean[MOOD_FEATURE_COUNT] = {};'.format(arr(model['mean'])),
        'static const float mood_model_std[MOOD_FEATURE_COUNT] = {};'.format(arr(model['std'])),
        'static const float mood_model_valence_w[MOOD_FEATURE_COUNT] = {};'.format(arr(model['wv'])),
        'static const float mood_model_valence_b = {:.5f}f;'.format(model['bv']),
        'static const float mood_model_energy_w[MOOD_FEATURE_COUNT] = {};'.format(arr(model['we'])),
        'static const float mood_model_energy_b = {:.5f}f;'.format(model['be']),
        '',
    ]
    with open(path, 'w') as f:
        f.write('\n'.join(lines))
    print('Wrote', path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv', nargs='+', help='labelled feature rows')
    ap.add_argument('-o', '--output', help='write the model header, e.g. ../main/mood_model.h')
    args = ap.parse_args()

    rows = load(args.csv)
    if not rows:
        sys.exit('no rows')
    if len({r[2] for r in rows}) > 1:
        evaluate(rows)
        print('Only build the fitted model in if it beats "current" above.')
    if args.output:
        write_header(fit(rows), args.output)


if __name__ == '__main__':
    main()

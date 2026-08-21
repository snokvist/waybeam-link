#!/usr/bin/env python3
"""Walk-test analyzer — #226 Leg A (probe under REAL rate-selective loss) and
§6.3b Phase E (salvage at real loss), from one walk.

  analyze.py <ground.ndjson> [craft.ndjson]

The ground log is this host's 2 Hz poll of /api/v1/stats. The craft log is the
SD-card file, one {"t_wall","t_up","s":<stats>} per line — it is the ONLY
source of promote_blocked_probe (§15.3 craft-only), which is the number the
walk exists to get.

Near-field warning: RSSI at bench range is compressed and can even read
positive. Samples above -20 dBm are reported separately and excluded from the
range bins, because a saturated front end inverts the evidence.
"""
import json, sys, math
from collections import defaultdict

SAT_DBM = -20  # above this the front end is compressed; not range data


def load(path, craft=False):
    out = []
    for ln in open(path):
        try:
            d = json.loads(ln)
        except Exception:
            continue
        if craft:
            if 'marker' in d:
                continue
            s = d.get('s') or {}
            l = s.get('link', {})
            a = (s.get('adapters') or [{}])[0]
            st = [x for x in s.get('streams', []) if x.get('stream_id') == 0]
            st = st[0] if st else {}
            r = {'t': d.get('t_wall'), 'up': d.get('t_up'), 'node': 'craft'}
            r.update({k: l.get(k) for k in (
                'profile', 'mcs', 'state', 'transition_reason', 'loss_ewma_milli',
                'probe_per', 'probe_per_age_ms', 'probe_candidate_mcs',
                'promote_blocked_probe', 'promote_blocked_saturated', 'tx_power_qdb')})
            r['a_rssi_best'] = a.get('rssi_best')
            r['a_snr'] = a.get('snr')
            r['s_delivered'] = st.get('delivered')
            out.append(r)
        else:
            out.append(d)
    return out


def bucket(rssi):
    if rssi is None:
        return None
    if rssi > SAT_DBM:
        return 'near-field'
    return '%d..%d' % (int(math.floor(rssi / 10.0)) * 10, int(math.floor(rssi / 10.0)) * 10 + 9)


def main():
    g = load(sys.argv[1])
    c = load(sys.argv[2], craft=True) if len(sys.argv) > 2 else []
    for node in sorted({r.get('node') for r in g if r.get('node')}):
        rows = [r for r in g if r.get('node') == node]
        print('\n=== %s — %d samples, %.0f s ===' % (node, len(rows), rows[-1]['t'] - rows[0]['t']))
        rs = [r['a_rssi_best'] for r in rows if r.get('a_rssi_best') is not None]
        print('  RSSI  min %s  max %s  |  %d samples in near field (> %d dBm), excluded below'
              % (min(rs) if rs else '-', max(rs) if rs else '-',
                 sum(1 for x in rs if x > SAT_DBM), SAT_DBM))

        # --- #226: probe_per vs candidate MCS, per RSSI bucket ---------------
        by = defaultdict(list)
        for r in rows:
            b = bucket(r.get('a_rssi_best'))
            p, cand = r.get('probe_per'), r.get('probe_candidate_mcs')
            if b is None or b == 'near-field' or cand in (None, 255):
                continue
            if p is None or p == 65535:
                by[(b, cand)].append(None)      # no opinion
            else:
                by[(b, cand)].append(p)
        if by:
            print('  §9.4 probe_per by RSSI bucket and candidate MCS'
                  '  (n = samples, "none" = 65535/no opinion):')
            print('    %-12s %-5s %6s %8s %8s %8s %8s' %
                  ('rssi', 'cand', 'n', 'none%', 'median', 'p90', 'max'))
            for (b, cand) in sorted(by, key=lambda k: (k[0], k[1])):
                v = by[(b, cand)]
                real = sorted(x for x in v if x is not None)
                nonepct = 100.0 * (len(v) - len(real)) / len(v)
                if real:
                    med = real[len(real) // 2]
                    p90 = real[min(len(real) - 1, int(len(real) * 0.9))]
                    mx = real[-1]
                else:
                    med = p90 = mx = '-'
                print('    %-12s %-5s %6d %7.0f%% %8s %8s %8s'
                      % (b, cand, len(v), nonepct, med, p90, mx))
        # --- profile trajectory ---------------------------------------------
        tr, last = [], None
        for r in rows:
            if r.get('profile') != last:
                tr.append((r['t'] - rows[0]['t'], r.get('profile'), r.get('a_rssi_best'),
                           r.get('transition_reason')))
                last = r.get('profile')
        print('  profile changes: %d' % len(tr))
        for t, p, rssi, why in tr[:24]:
            print('    +%5.0fs  prof=%-3s rssi=%-5s %s' % (t, p, rssi, why))
        if len(tr) > 24:
            print('    ... %d more' % (len(tr) - 24))
        # --- §6.3b -----------------------------------------------------------
        f = rows[-1]
        print('  §6.3b at end: salvaged=%s frozen=%s failed=%s slices=%s unrecoverable=%s '
              '| delivered=%s fec=%s'
              % (f.get('s_frames_salvaged'), f.get('s_frames_frozen'), f.get('s_salvage_failed'),
                 f.get('s_slices_synthesized'), f.get('s_frames_unrecoverable'),
                 f.get('s_delivered'), f.get('s_recovered_fec')))

    # --- the craft-only number ----------------------------------------------
    if c:
        print('\n=== craft (SD) — %d samples ===' % len(c))
        blocked = [r for r in c if (r.get('promote_blocked_probe') or 0) > 0]
        if blocked:
            first, last = blocked[0], blocked[-1]
            print('  *** VETO FIRED ON A REAL LINK ***')
            print('  promote_blocked_probe: 0 -> %s' % last.get('promote_blocked_probe'))
            print('  first at up=%.0fs  rssi=%s  prof=%s  probe_per=%s'
                  % (first.get('up') or 0, first.get('a_rssi_best'), first.get('profile'),
                     first.get('probe_per')))
        else:
            print('  promote_blocked_probe stayed 0 — the veto did not fire on this walk.')
        sat = [r for r in c if (r.get('promote_blocked_saturated') or 0) > 0]
        print('  promote_blocked_saturated: %s'
              % (sat[-1].get('promote_blocked_saturated') if sat else 0))
        pw = {r.get('tx_power_qdb') for r in c}
        print('  tx_power_qdb seen: %s' % sorted(x for x in pw if x is not None))


main()

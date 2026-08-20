#!/usr/bin/env python3
"""salvage_sim.py — end-to-end offline simulation of the production pipeline:

  x265 multi-slice HEVC -> per-frame blob [8B meta][Annex-B]
  -> chunk into s-byte source symbols (k = ceil(len/s)), r repair symbols
  -> random symbol loss (Bernoulli or burst)
  -> FEC model: received src+repair >= k  => full recovery (MDS)
                else                      => salvage received source symbols
  -> slice-completeness from surviving byte ranges + learned geometry
  -> synthesize concealment slices for missing ones, rebuild AU
  -> whole stream decode + per-frame region validation

Usage: salvage_sim.py <stream.265> <s_bytes> <r_per_frame> <loss_pct> <seed> <out.265>
"""
import sys, random
import hevc_ms as H

META = 8  # VencFrameMeta bytes prepended to each AU blob


class Geometry:
    """Learned slice geometry: expected segment addresses (from the most
    recent frame whose slices were all parseable), plus a cached donor header
    and last emitted POC for whole-frame concealment."""
    def __init__(self):
        self.addrs = None
        self.donor = None      # most recent P slice header seen complete
        self.last_poc = None   # poc_lsb of last emitted frame

    def learn(self, addrs):
        self.addrs = list(addrs)


def present_ranges(bitmap, s, total):
    out = []
    start = None
    for i, p in enumerate(bitmap):
        if p and start is None:
            start = i * s
        if not p and start is not None:
            out.append((start, i * s))
            start = None
    if start is not None:
        out.append((start, total))
    return out


def find_start_codes(blob, ranges):
    """Scan only inside present ranges; return offsets of NAL starts (offset of
    first byte after the 00 00 01)."""
    hits = []
    for (a, b) in ranges:
        i = a
        while i + 3 <= b:
            if blob[i] == 0 and blob[i + 1] == 0 and blob[i + 2] == 1:
                hits.append((i, i + 3))
                i += 3
            else:
                i += 1
    return hits


def range_containing(ranges, off):
    for r in ranges:
        if r[0] <= off < r[1]:
            return r
    return None


def salvage_frame(blob, bitmap, s, sps, pps_map, geom, stats):
    """Return repaired Annex-B AU bytes (without meta), or None (drop)."""
    total = len(blob)
    ranges = present_ranges(bitmap, s, total)
    scs = find_start_codes(blob, ranges)
    # parse VCL slice headers among survivors
    survivors = []  # (sc_off, nal_off, addr, header)
    non_vcl = []    # (sc_off, nal_off, type) prefix NALs (VPS/SPS/PPS/SEI/AUD)
    for sc, no in scs:
        if no + 3 > total:
            continue
        r = range_containing(ranges, no)
        if r is None or no + 8 > r[1]:
            continue  # header bytes not fully present -> incomplete anyway
        t = H.nal_type(blob[no:no + 2] + b"\x00")
        if H.IS_VCL(t):
            end = next((h[0] for h in scs if h[0] > sc), total)
            same = range_containing(ranges, sc)
            complete = same is not None and end <= same[1] and \
                (end < total or same[1] >= total)
            try:
                hdr = H.parse_slice_header(bytes(blob[no:min(no + 256, r[1])]),
                                           sps, pps_map)
            except Exception:
                continue
            survivors.append((sc, no, end, hdr, complete, t))
        else:
            non_vcl.append((sc, no, t))
    vcl_complete = [(sv[3].address, sv) for sv in survivors if sv[4]]
    if not vcl_complete:
        # whole-frame concealment: no surviving slice at all. Use the cached
        # donor from a previous P picture, advancing POC by one.
        if geom.donor is None or geom.addrs is None or geom.last_poc is None:
            stats["frames_dropped"] += 1
            return None
        pps = pps_map[geom.donor.pps_id]
        poc = (geom.last_poc + 1) % (1 << sps["log2_max_poc_lsb"])
        out = bytearray()
        for i, addr in enumerate(geom.addrs):
            end_addr = geom.addrs[i + 1] if i + 1 < len(geom.addrs) \
                else sps["pic_size_ctbs"]
            out += H.make_conceal_slice(sps, pps, geom.donor, addr,
                                        end_addr - addr,
                                        is_first_slice=(i == 0), poc_lsb=poc)
            stats["slices_replaced"] += 1
        stats["frames_frozen"] += 1
        geom.last_poc = poc
        return bytes(out)
    hdrs = [sv[3] for _, sv in vcl_complete]
    if any(H.IS_IRAP(sv[5]) for _, sv in vcl_complete):
        # incomplete IRAP: never conceal intra pictures
        stats["frames_dropped"] += 1
        return None
    if geom.addrs is None:
        stats["frames_dropped"] += 1
        return None
    got = {a for a, _ in vcl_complete}
    if not got.issubset(set(geom.addrs)):
        stats["frames_dropped"] += 1  # geometry mismatch -> fail safe
        return None
    donor = hdrs[0]
    pps = pps_map[donor.pps_id]
    out = bytearray()
    addr_list = geom.addrs
    for i, addr in enumerate(addr_list):
        end_addr = addr_list[i + 1] if i + 1 < len(addr_list) else sps["pic_size_ctbs"]
        if addr in got:
            sv = next(sv for a, sv in vcl_complete if a == addr)
            out += b"\x00\x00\x00\x01" + bytes(blob[sv[1]:sv[2]])
            stats["slices_original"] += 1
        else:
            out += H.make_conceal_slice(sps, pps, donor, addr, end_addr - addr,
                                        is_first_slice=(i == 0))
            stats["slices_replaced"] += 1
    stats["frames_repaired"] += 1
    if donor.slice_type == 1:
        geom.donor = donor
    geom.last_poc = donor.poc_lsb
    return bytes(out)


def main():
    path, s, r_count, loss_pct, seed, outp = sys.argv[1:7]
    s, r_count, seed = int(s), int(r_count), int(seed)
    loss = float(loss_pct) / 100.0
    rng = random.Random(seed)
    data = open(path, "rb").read()
    sps_map, pps_map = H.stream_context(data)
    sps = list(sps_map.values())[0]
    aus = H.split_aus(data)
    geom = Geometry()
    stats = dict(frames_total=0, frames_clean=0, frames_fec=0,
                 frames_repaired=0, frames_frozen=0, frames_dropped=0,
                 slices_original=0, slices_replaced=0,
                 src_received=0, src_lost=0)
    out = bytearray()
    for au in aus:
        stats["frames_total"] += 1
        annexb = au.rebuild()
        blob = bytes(META) + annexb  # 8B fake meta prefix
        k = (len(blob) + s - 1) // s
        # IRAP AUs are modeled as fully protected: production gives IDR frames
        # i_rate FEC + ARQ retransmission until delivered, and the decoder
        # gate holds until one arrives. Concealment never applies to intra.
        irap = any(H.IS_IRAP(H.nal_type(au.nals[i][0])) for i in au.slices)
        # symbol loss
        src_ok = [irap or rng.random() >= loss for _ in range(k)]
        rep_ok = sum(rng.random() >= loss for _ in range(r_count))
        stats["src_received"] += sum(src_ok)
        stats["src_lost"] += k - sum(src_ok)
        # learn geometry from frames that would decode header-complete
        def learn():
            try:
                hs = [H.parse_slice_header(au.nals[i][0], sps, pps_map)
                      for i in au.slices]
                if all(not h.dependent for h in hs):
                    geom.learn([h.address for h in hs])
                if hs and hs[0].slice_type == 1:
                    geom.donor = hs[0]
                if hs and hs[0].poc_lsb is not None:
                    geom.last_poc = hs[0].poc_lsb
                elif irap:
                    geom.last_poc = 0  # IDR resets POC
            except Exception:
                pass
        if all(src_ok):
            stats["frames_clean"] += 1
            learn()
            out += annexb
            continue
        if sum(src_ok) + rep_ok >= k:
            stats["frames_fec"] += 1
            learn()
            out += annexb
            continue
        bitmap = src_ok
        # meta bytes occupy blob[0:8]; annexb starts at 8. Shift ranges.
        rep = salvage_frame_wrap(blob, bitmap, s, sps, pps_map, geom, stats)
        if rep is not None:
            out += rep
    open(outp, "wb").write(bytes(out))
    print(stats)
    print(f"wrote {outp} ({len(out)} B)")


def salvage_frame_wrap(blob, bitmap, s, sps, pps_map, geom, stats):
    return salvage_frame(blob, bitmap, s, sps, pps_map, geom, stats)


if __name__ == "__main__":
    main()

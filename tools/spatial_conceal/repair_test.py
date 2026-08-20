#!/usr/bin/env python3
"""repair_test.py — offline validation of concealment slice substitution.

Usage: repair_test.py <stream.265> <frame_idx> <slice_idx[,slice_idx...]> <out.265>

Replaces the given slice(s) of the given displayed-frame AU with generated
all-skip concealment slices, writes the repaired stream.
"""
import sys
import hevc_ms as H


def analyze(path):
    data = open(path, "rb").read()
    sps_map, pps_map = H.stream_context(data)
    aus = H.split_aus(data)
    return data, sps_map, pps_map, aus


def slice_geometry(au, sps_map, pps_map):
    """Return (sps, pps, headers[], addresses[], ctb_counts[]) for the AU."""
    headers = []
    for si in au.slices:
        nal, _ = au.nals[si]
        sps = list(sps_map.values())[0]
        h = H.parse_slice_header(nal, sps, pps_map)
        headers.append(h)
    pps = pps_map[headers[0].pps_id]
    sps = sps_map[pps["sps_id"]]
    addrs = [h.address for h in headers]
    counts = []
    for i, a in enumerate(addrs):
        end = addrs[i + 1] if i + 1 < len(addrs) else sps["pic_size_ctbs"]
        counts.append(end - a)
    return sps, pps, headers, addrs, counts


def main():
    path, frame_idx, slice_spec, out = sys.argv[1:5]
    frame_idx = int(frame_idx)
    kill = sorted(set(int(x) for x in slice_spec.split(",")))
    data, sps_map, pps_map, aus = analyze(path)
    au = aus[frame_idx]
    sps, pps, headers, addrs, counts = slice_geometry(au, sps_map, pps_map)
    print(f"AU {frame_idx}: {len(au.nals)} NALs, {len(au.slices)} slices, "
          f"types {[H.nal_type(au.nals[i][0]) for i in au.slices]}, "
          f"addrs {addrs}, ctbs {counts}, poc_lsb {headers[0].poc_lsb}")
    donor_idx = next((i for i in range(len(headers)) if i not in kill), None)
    poc_override = None
    if donor_idx is None:
        # whole frame lost: donor from a previous P AU, POC advanced
        for back in range(frame_idx - 1, -1, -1):
            _, _, ph, paddrs, pcounts = slice_geometry(aus[back], sps_map, pps_map)
            if ph[0].poc_lsb is not None:
                donor = ph[0]
                poc_override = (headers[0].poc_lsb if headers else
                                (donor.poc_lsb + (frame_idx - back)) %
                                (1 << sps["log2_max_poc_lsb"]))
                print(f"  whole-frame conceal: donor from AU {back}, "
                      f"poc_lsb {poc_override}")
                break
    else:
        donor = headers[donor_idx]
    for k in kill:
        orig_len = len(au.nals[au.slices[k]][0])
        new = H.make_conceal_slice(sps, pps, donor, addrs[k], counts[k],
                                   is_first_slice=(k == 0),
                                   poc_lsb=poc_override)
        # strip the 4-byte start code we added; AU rebuild adds one
        au.nals[au.slices[k]] = (new[4:], 4)
        print(f"  slice {k}: {orig_len} B -> {len(new) - 4} B concealment "
              f"(addr {addrs[k]}, {counts[k]} CTBs)")
    blob = bytearray()
    for i, a in enumerate(aus):
        blob += a.rebuild()
    open(out, "wb").write(bytes(blob))
    print(f"wrote {out} ({len(blob)} B)")


if __name__ == "__main__":
    main()

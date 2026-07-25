#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
mode_harness.py — operating-mode verification harness (docs/venc-mode-matrix.md §16).

Runs on the GROUND host and drives the craft over SSH. It exercises the
mode/pin control plane end-to-end and checks the one property a unit test
cannot: that after a venc restart the encoder is NOT stranded at a stale
bitrate (§15.5 Pass 103). The stranding detector is two INDEPENDENT signals —
the craft's commanded `link.venc_bitrate_kbps` (what the link asked for) vs the
ground's DELIVERED video bitrate (what the encoder actually produced, measured
from `streams[].frame_bytes`). A stranded encoder shows commanded-high /
delivered-near-zero; a healthy one shows delivered ~= commanded.

Supported mode-apply entry is POST /api/v1/mode (the hub path); this harness
drives exactly that. It never edits config by hand.

Phases (select with --phases, default all):
  preflight     both nodes reachable, link up, video flowing; snapshot baseline
  mode_sweep    apply each mode via /api/v1/mode; assert pin + not-stranded
  mcs_walk      pin min==max across the band up then down; assert mcs/bitrate track
  venc_restart  manufacture a stranded encoder, restart venc, reassert; assert recovery
  mode_reapply  manufacture stranding, re-apply the current mode (same rung); recovery
  variable_fps  imx335-variable: §9.11 ladder runs and survives the reassert
  link_restart  restart waybeam-link; assert it boots the persisted mode + recovers
  full_reboot   restart venc+link together; assert no stale/undefined state

Exit code is non-zero if any check fails. Read-only phases leave the craft on
its baseline mode; the harness restores it on exit.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

# ---- config / plumbing -----------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--craft-ssh", default="root@192.168.2.232",
                   help="ssh target for the craft (default: %(default)s)")
    p.add_argument("--craft-ctrl", default="localhost:8091",
                   help="craft control bind, curl'd ON the craft (default: %(default)s)")
    p.add_argument("--ground-ctrl", default="127.0.0.1:8092",
                   help="ground control bind, curl'd locally (default: %(default)s)")
    p.add_argument("--video-stream", type=int, default=0,
                   help="ground stream_id carrying video (default: %(default)s)")
    p.add_argument("--originator", type=int, default=17,
                   help="craft originator for scout/quickconnect (default: %(default)s)")
    p.add_argument("--modes-dir", default="profiles/modes",
                   help="local dir with mode JSONs for expected pins (default: %(default)s)")
    p.add_argument("--modes", default="",
                   help="comma list of modes for mode_sweep; empty = every modes-dir json")
    p.add_argument("--phases",
                   default="preflight,mode_sweep,mcs_walk,venc_restart,mode_reapply,"
                           "variable_fps,link_restart,full_reboot",
                   help="comma list of phases to run")
    p.add_argument("--settle", type=float, default=9.0,
                   help="seconds to wait after a venc restart before measuring")
    p.add_argument("--pin-settle", type=float, default=3.0,
                   help="seconds to wait after a live pin before measuring")
    p.add_argument("--window", type=float, default=4.0,
                   help="seconds over which to measure delivered bitrate")
    p.add_argument("--restart-timeout", type=float, default=45.0,
                   help="seconds to wait for a restarted process to serve again")
    p.add_argument("--strand-frac", type=float, default=0.5,
                   help="delivered must be >= frac * commanded (else: stranded)")
    p.add_argument("--min-kbps", type=float, default=500.0,
                   help="absolute delivered-bitrate floor (kbps)")
    p.add_argument("--venc-init", default="/etc/init.d/S95waybeam")
    p.add_argument("--link-init", default="/etc/init.d/S96waybeam-link")
    p.add_argument("--venc-http", default="localhost",
                   help="venc httpd host on the craft (default: %(default)s = :80)")
    p.add_argument("--strand-sentinel", type=int, default=1500,
                   help="low bitrate persisted to venc to manufacture stranding (kbps)")
    p.add_argument("--no-manufacture", dest="manufacture", action="store_false",
                   help="do not persist the sentinel; rely on the natural disk value")
    p.add_argument("--dry-run", action="store_true",
                   help="print the actions without applying/restarting anything")
    return p.parse_args()


class Fail(Exception):
    pass


def run(cmd, timeout=20):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


class Craft:
    """Talks to the craft control plane over `ssh <target> curl localhost:...`."""
    def __init__(self, args):
        self.ssh = args.craft_ssh
        self.ctrl = args.craft_ctrl
        self.venc_http = args.venc_http
        self.dry = args.dry_run

    def _ssh(self, remote_cmd, timeout=20):
        return run(["ssh", "-o", "ConnectTimeout=5", "-o", "BatchMode=yes",
                    self.ssh, remote_cmd], timeout=timeout)

    def get(self, path):
        rc, out, err = self._ssh(f"curl -s http://{self.ctrl}{path}")
        if rc != 0:
            raise Fail(f"craft GET {path}: ssh rc={rc} {err.strip()}")
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            raise Fail(f"craft GET {path}: non-JSON: {out[:120]!r}")

    def post(self, path, body):
        payload = json.dumps(body)
        if self.dry:
            print(f"    [dry-run] craft POST {path} {payload}")
            return {}
        # single-quote the JSON for the remote shell; JSON has no single quotes.
        rc, out, err = self._ssh(
            f"curl -s -X POST http://{self.ctrl}{path} "
            f"-H 'Content-Type: application/json' -d '{payload}'")
        if rc != 0:
            raise Fail(f"craft POST {path}: ssh rc={rc} {err.strip()}")
        try:
            return json.loads(out) if out.strip() else {}
        except json.JSONDecodeError:
            return {"raw": out}

    def sh(self, remote_cmd, timeout=40):
        if self.dry:
            print(f"    [dry-run] craft sh: {remote_cmd}")
            return 0, "", ""
        return self._ssh(remote_cmd, timeout=timeout)

    def venc_persist(self, kbps, timeout=30):
        """Persist a bitrate straight to venc (the /set path the link's fallback
        uses). Manufactures a known-bad on-disk value so a restart deterministically
        boots stranded, independent of whatever the link last wrote. Retries while
        venc's httpd is still registering routes after a restart (its bring-up
        404s — 'no matching route')."""
        if self.dry:
            print(f"    [dry-run] venc persist video0.bitrate={kbps}")
            return
        deadline = time.monotonic() + timeout
        last = ""
        while time.monotonic() < deadline:
            rc, out, err = self._ssh(
                f"curl -s 'http://{self.venc_http}/api/v1/set?video0.bitrate={kbps}'")
            if rc == 0 and '"ok":true' in out:
                return
            last = out or err
            time.sleep(2)
        raise Fail(f"venc persist {kbps}: not accepted within {timeout:.0f}s ({last[:80]!r})")

    def link(self):
        return self.get("/api/v1/stats").get("link", {})

    def active_mode(self):
        return self.get("/api/v1/mode").get("active", "")


class Ground:
    def __init__(self, args):
        self.ctrl = args.ground_ctrl
        self.vstream = args.video_stream
        self.dry = args.dry_run

    def get(self, path):
        rc, out, err = run(["curl", "-s", f"http://{self.ctrl}{path}"])
        if rc != 0:
            raise Fail(f"ground GET {path}: curl rc={rc} {err.strip()}")
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            raise Fail(f"ground GET {path}: non-JSON: {out[:120]!r}")

    def post(self, path, body):
        payload = json.dumps(body)
        if self.dry:
            print(f"    [dry-run] ground POST {path} {payload}")
            return {}
        rc, out, err = run(["curl", "-s", "-X", "POST", f"http://{self.ctrl}{path}",
                            "-H", "Content-Type: application/json", "-d", payload])
        if rc != 0:
            raise Fail(f"ground POST {path}: curl rc={rc} {err.strip()}")
        try:
            return json.loads(out) if out.strip() else {}
        except json.JSONDecodeError:
            return {"raw": out}

    def _video_frame_bytes(self):
        for s in self.get("/api/v1/stats").get("streams", []):
            if s.get("stream_id") == self.vstream:
                return int(s.get("frame_bytes", 0))
        raise Fail(f"ground stats has no stream_id {self.vstream}")

    def delivered_kbps(self, window):
        """Measure delivered video bitrate over `window` seconds (frame_bytes delta)."""
        b0 = self._video_frame_bytes()
        t0 = time.monotonic()
        time.sleep(window)
        b1 = self._video_frame_bytes()
        dt = time.monotonic() - t0
        return (b1 - b0) * 8.0 / dt / 1000.0


# ---- helpers ---------------------------------------------------------------

def load_expected_pins(modes_dir):
    """{mode_name: (min_profile, max_profile, fps, size)} from local mode JSONs."""
    pins = {}
    for f in sorted(Path(modes_dir).glob("*.json")):
        try:
            d = json.loads(f.read_text())
        except json.JSONDecodeError:
            continue
        sel = d.get("link", {}).get("policy", {}).get("select", {})
        v = d.get("venc", {}).get("video0", {})
        if "min_profile" in sel and "max_profile" in sel:
            pins[f.stem] = (sel["min_profile"], sel["max_profile"],
                            v.get("fps"), v.get("size"))
    return pins


def wait_serving(getter, timeout, what):
    """Poll `getter()` until it returns without raising, or timeout."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return getter()
        except Fail as e:
            last = e
            time.sleep(1.5)
    raise Fail(f"{what}: not serving after {timeout:.0f}s ({last})")


def wait_active_mode(craft, name, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if craft.active_mode() == name:
                return
        except Fail:
            pass
        time.sleep(1.5)
    raise Fail(f"active mode never became {name!r} within {timeout:.0f}s")


def not_stranded(commanded, delivered, args):
    """The core check: delivered video must track the commanded bitrate."""
    floor = max(args.min_kbps, args.strand_frac * commanded)
    ok = delivered >= floor
    detail = (f"commanded={commanded:.0f}k delivered={delivered:.0f}k "
              f"(floor={floor:.0f}k, ratio={delivered/commanded if commanded else 0:.2f})")
    return ok, detail


# ---- phases ----------------------------------------------------------------

class Harness:
    def __init__(self, args):
        self.args = args
        self.craft = Craft(args)
        self.ground = Ground(args)
        self.pins = load_expected_pins(args.modes_dir)
        self.baseline_mode = None
        self.results = []  # (phase, name, ok, detail)

    def record(self, phase, name, ok, detail):
        self.results.append((phase, name, ok, detail))
        flag = "PASS" if ok else "FAIL"
        print(f"  [{flag}] {phase}/{name}: {detail}")

    def wait_commanded(self, timeout):
        """Poll until the link reports a non-zero commanded bitrate. After a mode
        apply the applier POSTs /venc/reassert, which drops the actuator cache —
        so commanded reads 0 for the sub-second window until the re-push lands.
        Returns the commanded kbps (0 if it never came up)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            c = float(self.craft.link().get("venc_bitrate_kbps", 0))
            if c > 0:
                return c
            time.sleep(0.5)
        return 0.0

    def wait_recover(self, commanded, timeout):
        """Poll delivered bitrate until it clears the not-stranded floor, or
        timeout. Separates encoder bring-up lag (recovers within the window)
        from genuine stranding (never recovers). Returns (ok, delivered, secs)."""
        a = self.args
        floor = max(a.min_kbps, a.strand_frac * commanded)
        t0 = time.monotonic()
        delivered = 0.0
        while time.monotonic() - t0 < timeout:
            delivered = self.ground.delivered_kbps(min(a.window, 2.5))
            if delivered >= floor:
                return True, delivered, time.monotonic() - t0
        return False, delivered, time.monotonic() - t0

    def rescout(self):
        """Re-establish the ground->craft latch after a craft-side restart (B9:
        the craft's CSA session changes, so a claim must be re-issued). On the
        same home channel plain video RX usually re-latches on its own; this is
        a best-effort nudge when it does not."""
        a = self.args
        try:
            self.ground.post("/api/v1/scout/start",
                             {"mode": "quickconnect", "target": {"originator": a.originator}})
            time.sleep(3)
            self.ground.post("/api/v1/scout/quickconnect", {"originator": a.originator})
            time.sleep(3)
        except Fail as e:
            print(f"    (rescout best-effort failed: {e})")

    # -- preflight
    def phase_preflight(self):
        link = wait_serving(self.craft.link, self.args.restart_timeout, "craft control")
        self.baseline_mode = self.craft.active_mode()
        commanded = float(link.get("venc_bitrate_kbps", 0))
        delivered = self.ground.delivered_kbps(self.args.window)
        ok = delivered >= self.args.min_kbps and commanded > 0
        self.record("preflight", "link+video",
                    ok, f"baseline_mode={self.baseline_mode} state={link.get('state')} "
                        f"mcs={link.get('mcs')} commanded={commanded:.0f}k "
                        f"delivered={delivered:.0f}k")
        if not ok:
            raise Fail("preflight failed — no healthy video baseline; aborting")

    # -- mode sweep (the production stranding test)
    def phase_mode_sweep(self):
        a = self.args
        names = ([m.strip() for m in a.modes.split(",") if m.strip()]
                 or sorted(self.pins.keys()))
        for name in names:
            if name not in self.pins:
                self.record("mode_sweep", name, False, "no local mode json / no pin")
                continue
            mn, mx, fps, size = self.pins[name]
            self.craft.post("/api/v1/mode", {"name": name})
            try:
                wait_active_mode(self.craft, name, a.restart_timeout)
            except Fail as e:
                self.record("mode_sweep", name, False, str(e))
                continue
            time.sleep(a.settle)
            # Let the reassert re-push land (commanded reads 0 in that window),
            # then poll delivered up to the not-stranded floor.
            commanded = self.wait_commanded(a.restart_timeout)
            ok_str, delivered, secs = self.wait_recover(commanded, a.restart_timeout)
            prof = self.craft.link().get("profile")
            ok_pin = prof is not None and mn <= prof <= mx
            self.record("mode_sweep", name, ok_pin and ok_str,
                        f"pin[{mn},{mx}] profile={prof} {size}@{fps} "
                        f"commanded={commanded:.0f}k delivered={delivered:.0f}k in {secs:.0f}s")

    # -- mcs walk (pin min==max across the band, up then down)
    def phase_mcs_walk(self):
        a = self.args
        mode = self.craft.active_mode()
        band = self.pins.get(mode)
        if not band:
            self.record("mcs_walk", mode, False, "current mode has no known band")
            return
        mn, mx = band[0], band[1]
        rungs = list(range(mn, mx + 1))
        walk = rungs + rungs[-2::-1]  # up then back down, no repeat at the top
        prev_delivered = None
        try:
            for i, rung in enumerate(walk):
                self.craft.post("/api/v1/link/profile", {"min": rung, "max": rung})
                time.sleep(a.pin_settle)
                link = self.craft.link()
                prof = link.get("profile")
                mcs = link.get("mcs")
                commanded = float(link.get("venc_bitrate_kbps", 0))
                delivered = self.ground.delivered_kbps(a.window)
                ok_pin = prof == rung
                ok_str, sdet = not_stranded(commanded, delivered, a)
                # Monotonicity is advisory (a clean link may already sit high);
                # log it but don't fail the rung on it.
                mono = "" if prev_delivered is None else \
                    (" up" if delivered >= prev_delivered * 0.8 else " (down)")
                prev_delivered = delivered
                self.record("mcs_walk", f"rung{rung}", ok_pin and ok_str,
                            f"profile={prof} mcs={mcs}{mono} — {sdet}")
        finally:
            # restore the band pin (unpin to [mn,mx]).
            self.craft.post("/api/v1/link/profile", {"min": mn, "max": mx})

    # -- venc restart alone: the reassert endpoint must recover a manufactured
    #    stranded encoder. This is the deterministic §15.5 Pass 103 regression:
    #    the pre-fix binary has no /venc/reassert and stays stranded (RED); the
    #    fixed binary re-asserts the commanded bitrate (GREEN).
    def phase_venc_restart(self):
        a = self.args
        commanded = float(self.craft.link().get("venc_bitrate_kbps", 0))
        if a.manufacture:
            self.craft.venc_persist(a.strand_sentinel)  # boot venc low on next start
        self.craft.sh(f"{a.venc_init} restart")
        wait_serving(self.craft.link, a.restart_timeout, "craft after venc restart")
        self.craft.post("/api/v1/venc/reassert", {})  # 404 on pre-fix → stays stranded
        commanded = float(self.craft.link().get("venc_bitrate_kbps", commanded))
        ok, delivered, secs = self.wait_recover(commanded, a.restart_timeout)
        self.record("venc_restart", "reassert-recovers", ok,
                    f"manufactured={a.strand_sentinel if a.manufacture else 'off'}k "
                    f"commanded={commanded:.0f}k delivered={delivered:.0f}k in {secs:.0f}s")

    # -- variable-fps mode (§9.11 ladder). Unlike the nine static modes, the
    #    applier turns the fps ladder ON (link_fps true) after the venc restart,
    #    then reasserts. Verify the ladder is running AND that a manufactured
    #    strand + reapply recovers the bitrate WITHOUT killing the ladder (the
    #    reassert also drops the fps cache, so the ladder must keep driving fps).
    def phase_variable_fps(self, name="imx335-variable"):
        a = self.args
        if name not in self.pins:
            self.record("variable_fps", name, False, "no local variable mode json")
            return
        self.craft.post("/api/v1/mode", {"name": name})
        try:
            wait_active_mode(self.craft, name, a.restart_timeout)
        except Fail as e:
            self.record("variable_fps", name, False, str(e))
            return
        time.sleep(a.settle)
        link = self.craft.link()
        ladder_on = bool(link.get("cmd_fps_ladder"))
        fps0 = link.get("venc_fps")
        self.record("variable_fps", "ladder-running", ladder_on,
                    f"cmd_fps_ladder={ladder_on} state={link.get('venc_fps_ladder_state')} "
                    f"venc_fps={fps0}")
        # Now strand + reapply through the full production path.
        commanded = float(link.get("venc_bitrate_kbps", 0))
        if a.manufacture:
            self.craft.venc_persist(a.strand_sentinel)
        self.craft.post("/api/v1/mode", {"name": name})
        wait_active_mode(self.craft, name, a.restart_timeout)
        commanded = self.wait_commanded(a.restart_timeout) or commanded
        ok_str, delivered, secs = self.wait_recover(commanded, a.restart_timeout)
        link = self.craft.link()
        ladder_still = bool(link.get("cmd_fps_ladder"))
        fps1 = link.get("venc_fps")
        self.record("variable_fps", "reassert-keeps-ladder", ok_str and ladder_still,
                    f"manufactured={a.strand_sentinel if a.manufacture else 'off'}k "
                    f"commanded={commanded:.0f}k delivered={delivered:.0f}k in {secs:.0f}s "
                    f"ladder={ladder_still} venc_fps={fps1}")

    # -- full production path: manufacture stranding, then re-apply the CURRENT
    #    mode via POST /api/v1/mode. The rung is unchanged, so the actuator has
    #    nothing it thinks is new — only the applier's reassert curl (apply-mode.sh
    #    step 6) closes the gap. Tests the hub path exactly.
    def phase_mode_reapply(self):
        a = self.args
        mode = self.craft.active_mode()
        commanded = float(self.craft.link().get("venc_bitrate_kbps", 0))
        if a.manufacture:
            self.craft.venc_persist(a.strand_sentinel)
        self.craft.post("/api/v1/mode", {"name": mode})
        try:
            wait_active_mode(self.craft, mode, a.restart_timeout)
        except Fail as e:
            self.record("mode_reapply", mode, False, str(e))
            return
        commanded = float(self.craft.link().get("venc_bitrate_kbps", commanded))
        ok, delivered, secs = self.wait_recover(commanded, a.restart_timeout)
        self.record("mode_reapply", f"{mode}-samerung", ok,
                    f"manufactured={a.strand_sentinel if a.manufacture else 'off'}k "
                    f"commanded={commanded:.0f}k delivered={delivered:.0f}k in {secs:.0f}s")

    # -- link restart: must boot the persisted mode and recover video
    def phase_link_restart(self):
        a = self.args
        want_mode = self.craft.active_mode()
        self.craft.sh(f"{a.link_init} restart")
        wait_serving(self.craft.link, a.restart_timeout, "craft after link restart")
        # CSA session changed (B9) — nudge the ground latch if video is dark.
        time.sleep(a.settle)
        if self.ground.delivered_kbps(2.0) < a.min_kbps:
            self.rescout()
        got_mode = self.craft.active_mode()
        commanded = float(self.craft.link().get("venc_bitrate_kbps", 0))
        ok_str, delivered, secs = self.wait_recover(commanded, a.restart_timeout)
        ok_mode = got_mode == want_mode
        self.record("link_restart", "persisted-mode+recover", ok_mode and ok_str,
                    f"mode want={want_mode} got={got_mode} commanded={commanded:.0f}k "
                    f"delivered={delivered:.0f}k in {secs:.0f}s")

    # -- full restart of both: no stale/undefined state
    def phase_full_reboot(self):
        a = self.args
        want_mode = self.craft.active_mode()
        self.craft.sh(f"{a.venc_init} stop")
        self.craft.sh(f"{a.link_init} restart")
        self.craft.sh(f"{a.venc_init} start")
        wait_serving(self.craft.link, a.restart_timeout, "craft after full restart")
        time.sleep(a.settle)
        if self.ground.delivered_kbps(2.0) < a.min_kbps:
            self.rescout()
        got_mode = self.craft.active_mode()
        commanded = float(self.craft.link().get("venc_bitrate_kbps", 0))
        ok_str, delivered, secs = self.wait_recover(commanded, a.restart_timeout)
        ok_mode = got_mode == want_mode
        self.record("full_reboot", "clean-boot", ok_mode and ok_str,
                    f"mode want={want_mode} got={got_mode} commanded={commanded:.0f}k "
                    f"delivered={delivered:.0f}k in {secs:.0f}s")

    # -- driver
    def run(self):
        phases = [p.strip() for p in self.args.phases.split(",") if p.strip()]
        # preflight must run first to capture the baseline for restore.
        if "preflight" not in phases:
            phases.insert(0, "preflight")
        try:
            for ph in phases:
                fn = getattr(self, f"phase_{ph}", None)
                if fn is None:
                    print(f"  (unknown phase {ph!r}, skipped)")
                    continue
                print(f"\n== phase: {ph} ==")
                try:
                    fn()
                except Fail as e:
                    self.record(ph, "phase", False, f"aborted: {e}")
                    if ph == "preflight":
                        break
        finally:
            self.restore_baseline()
        return self.summary()

    def restore_baseline(self):
        if self.baseline_mode and not self.args.dry_run:
            try:
                if self.craft.active_mode() != self.baseline_mode:
                    print(f"\nrestoring baseline mode {self.baseline_mode}")
                    self.craft.post("/api/v1/mode", {"name": self.baseline_mode})
                    wait_active_mode(self.craft, self.baseline_mode,
                                     self.args.restart_timeout)
            except Fail as e:
                print(f"  (baseline restore failed: {e})")

    def summary(self):
        npass = sum(1 for *_, ok, _ in self.results if ok)
        nfail = len(self.results) - npass
        print("\n" + "=" * 60)
        print(f"mode_harness: {npass} passed, {nfail} failed, {len(self.results)} checks")
        for phase, name, ok, detail in self.results:
            if not ok:
                print(f"  FAIL {phase}/{name}: {detail}")
        print("=" * 60)
        return 0 if nfail == 0 else 1


def main():
    args = parse_args()
    try:
        return Harness(args).run()
    except Fail as e:
        print(f"FATAL: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

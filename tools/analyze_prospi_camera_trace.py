"""Read-only ProSpi shot catalogue. Never writes a live calibration/profile file."""
import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path


SUSPECTS = {
    1: "invalid_camera", 2: "ambiguous_learned_match", 4: "large_safety_lift",
    8: "estimated_floor_unmet", 16: "safety_estimate_disagrees_with_measured_pose",
    32: "target_behind", 64: "target_offscreen", 128: "dolly_past_target",
    256: "static_path_obstruction", 512: "abrupt_assist_focus_change",
    1024: "stale_assist_snapshot",
}
PATH_FIELDS = ["seconds", "sequence", "epoch", "cut", "camera_sequence", "kind", "eye",
               "id", "play_mode", "raw_fov", "effective_fov", "base_fov", "focus_before",
               "focus_after", "dolly", "safety_up", "raw_x", "raw_y", "raw_z",
               "neutral_x", "neutral_y", "neutral_z", "output_x", "output_y", "output_z",
               "suspects", "snapshot_age_ms", "previous_assist_write", "input_matches_assist"]


def vector(pose):
    value = (pose or {}).get("location")
    if not isinstance(value, list) or len(value) != 3:
        return None
    if not all(isinstance(v, (float, int)) and math.isfinite(v) for v in value):
        return None
    return value


def names(bits):
    return [name for bit, name in SUSPECTS.items() if bits & bit]


def catalogue(session, output):
    session, output = Path(session).resolve(), Path(output).resolve()
    if output == session or output in session.parents or session in output.parents:
        raise ValueError("Choose a separate output directory, not the trace or profile directory")
    output.mkdir(parents=True, exist_ok=True)
    metadata = json.loads((session / "metadata.json").read_text(encoding="utf-8"))
    if metadata.get("schema") != 1:
        raise ValueError("Unsupported trace schema")
    cuts, missing, coverage = {}, Counter(), Counter()
    first_ns = None
    last_ns = 0
    markers = []
    malformed = 0
    points = defaultdict(list)
    observed_stadiums = set()
    with (output / "camera_paths.csv").open("w", newline="", encoding="utf-8") as path_file:
        writer = csv.DictWriter(path_file, fieldnames=PATH_FIELDS)
        writer.writeheader()
        with (session / "events.jsonl").open(encoding="utf-8") as stream:
            for line in stream:
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    malformed += 1
                    continue  # An interrupted final write must not invalidate the other cuts.
                time_ns = e.get("time_ns", 0)
                if not isinstance(time_ns, int) or time_ns <= 0:
                    continue
                first_ns = time_ns if first_ns is None else min(first_ns, time_ns)
                last_ns = max(last_ns, time_ns)
                kind = e.get("kind", "unknown")
                coverage[kind] += 1
                if kind == "gap":
                    missing[e.get("label", "gap")] += 1
                    continue
                key = (e.get("epoch", 0), e.get("cut_sequence", 0))
                cut = cuts.setdefault(key, {"epoch": key[0], "cut": key[1], "first_ns": time_ns,
                    "last_ns": time_ns, "samples": 0, "ids": Counter(), "modes": Counter(),
                    "suspects": Counter(), "marks": [], "candidates": {}, "ranges": {},
                    "views": Counter(), "cache_previous_write": 0, "game_cut": False,
                    "inferred_cut": False, "targets_verified": 0, "geometry_verified": 0,
                    "post_tick_fov_range": None, "projection_symmetric_fov_range": None,
                    "cache_probe_observations": 0})
                cut["first_ns"] = min(cut["first_ns"], time_ns)
                cut["last_ns"] = max(cut["last_ns"], time_ns)
                cut["samples"] += 1
                for name in names(e.get("suspects", 0)):
                    cut["suspects"][name] += 1
                cut["inferred_cut"] |= e.get("inferred_cut", False)
                if kind == "marker":
                    marker = {"time_ns": time_ns, "epoch": key[0], "cut": key[1],
                              "marker": e.get("marker"), "label": e.get("label", "")}
                    cut["marks"].append(marker)
                    markers.append(marker)
                c = e.get("assist", {})
                raw = c.get("input", {})
                raw_xyz = vector(raw)
                cut["game_cut"] |= c.get("cut_known", False) and c.get("cut", False)
                if kind == "camera":
                    cut["ids"][c.get("id", "unknown")] += 1
                    cut["modes"][str(c.get("play_mode", "unknown"))] += 1
                    cut["cache_previous_write"] += bool(c.get("previous_update_wrote_fov"))
                    for match in c.get("matches", []):
                        cut["candidates"][match.get("id", "unknown")] = match
                    for field in ("focus_before", "focus_after", "effective_fov", "base_fov", "safety_up", "dolly_after"):
                        value = c.get(field)
                        if isinstance(value, (float, int)) and math.isfinite(value):
                            bounds = cut["ranges"].setdefault(field, [value, value])
                            bounds[0], bounds[1] = min(bounds[0], value), max(bounds[1], value)
                    value = raw.get("fov")
                    if isinstance(value, (float, int)) and math.isfinite(value):
                        bounds = cut["ranges"].setdefault("raw_fov", [value, value])
                        bounds[0], bounds[1] = min(bounds[0], value), max(bounds[1], value)
                view = e.get("view", {})
                neutral_xyz = vector(view.get("neutral", {})) if view.get("neutral_valid") else None
                output_xyz = vector(view.get("output", {}))
                if kind == "view":
                    cut["views"][str(view.get("eye", "unknown"))] += 1
                    if view.get("input_matches_assist") and raw_xyz and neutral_xyz and len(points[key]) < 600:
                        points[key].append((raw_xyz, neutral_xyz))
                if kind == "observation":
                    observation = e.get("observation", {})
                    cached_fov = observation.get("cache", {}).get("fov")
                    if isinstance(cached_fov, (float, int)) and math.isfinite(cached_fov):
                        bounds = cut["post_tick_fov_range"] or [cached_fov, cached_fov]
                        cut["post_tick_fov_range"] = [min(bounds[0], cached_fov), max(bounds[1], cached_fov)]
                        cut["cache_probe_observations"] += 1
                    cut["targets_verified"] += isinstance(observation.get("ball"), list)
                    floor = observation.get("floor", {})
                    cut["geometry_verified"] += floor.get("query_valid", False) and floor.get("static_component", False)
                    if observation.get("stadium"):
                        observed_stadiums.add(observation["stadium"])
                if kind == "projection":
                    matrix = e.get("projection", [])
                    if isinstance(matrix, list) and len(matrix) == 16 and isinstance(matrix[0], (float, int)) and abs(matrix[0]) > 0:
                        fov_hint = math.degrees(2 * math.atan(1 / abs(matrix[0])))
                        bounds = cut["projection_symmetric_fov_range"] or [fov_hint, fov_hint]
                        cut["projection_symmetric_fov_range"] = [min(bounds[0], fov_hint), max(bounds[1], fov_hint)]
                if kind in ("camera", "view"):
                    row = {"seconds": time_ns / 1e9, "sequence": e.get("sequence"), "epoch": key[0],
                        "cut": key[1], "camera_sequence": e.get("camera_sequence"), "kind": kind,
                        "eye": view.get("eye"), "id": c.get("id"), "play_mode": c.get("play_mode"),
                        "raw_fov": raw.get("fov"), "effective_fov": c.get("effective_fov"),
                        "base_fov": c.get("base_fov"), "focus_before": c.get("focus_before"),
                        "focus_after": c.get("focus_after"), "dolly": c.get("dolly_after"),
                        "safety_up": c.get("safety_up"), "suspects": "|".join(names(e.get("suspects", 0))),
                        "snapshot_age_ms": view.get("snapshot_age_ms"),
                        "previous_assist_write": c.get("previous_update_wrote_fov")}
                    row["input_matches_assist"] = view.get("input_matches_assist")
                    for prefix, xyz in (("raw", raw_xyz), ("neutral", neutral_xyz), ("output", output_xyz)):
                        if xyz:
                            for axis, value in zip("xyz", xyz):
                                row[f"{prefix}_{axis}"] = value
                    writer.writerow(row)
    summaries = []
    identities = defaultdict(set)
    for key, cut in sorted(cuts.items(), key=lambda pair: pair[1]["first_ns"]):
        cut["start_seconds"] = (cut.pop("first_ns") - (first_ns or 0)) / 1e9
        cut["end_seconds"] = (cut.pop("last_ns") - (first_ns or 0)) / 1e9
        cut["review_priority"] = 3 * len(cut["suspects"]) + 20 * sum(m["marker"] == 1 for m in cut["marks"])
        for identity in cut["ids"]:
            identities[identity].add(tuple(cut["ranges"].get("raw_fov", [])))
        summaries.append(cut)
    ambiguous_ids = {key: sorted(value) for key, value in identities.items() if len(value) > 1}
    catalogue_data = {"schema": 1, "record_only": True, "session": metadata.get("session"),
        "coverage": coverage, "gaps": missing, "malformed_lines": malformed,
        "capabilities": metadata.get("capabilities", {}), "observed_stadiums": sorted(observed_stadiums),
        "duration_seconds": (last_ns - (first_ns or last_ns)) / 1e9,
        "multi_fov_ids": ambiguous_ids, "cuts": summaries}
    (output / "shot_catalogue.json").write_text(json.dumps(catalogue_data, indent=2), encoding="utf-8")
    # No fabricated dolly/aim/safety corrections and no accidental v1 calibration imports.
    candidates = {"schema": "prospi-camera-review-v1", "auto_apply": False, "calibration_import_compatible": False,
        "ball_follow_policy": metadata.get("ball_follow_policy"),
        "requires": ["Verified shot identity and original game FOV", "Direct target/ball provider for ball-follow cuts",
            "Measured stadium geometry for floor/dugout/stand constraints", "Runtime A/B before enabling corrections"],
        "review": [{"epoch": cut["epoch"], "cut": cut["cut"], "priority": cut["review_priority"],
            "reasons": list(cut["suspects"]), "matched_entries": list(cut["candidates"]),
            "manual_good": any(m["marker"] == 2 for m in cut["marks"]), "proposed_correction": None}
            for cut in sorted(summaries, key=lambda cut: -cut["review_priority"])]}
    (output / "candidate_review.json").write_text(json.dumps(candidates, indent=2), encoding="utf-8")
    report = ["# ProSpi Camera Trace Review", "", "Record-only: no live camera settings or calibration entries were changed.", "",
        f"Stadium label: {metadata.get('stadium', 'unspecified')}. Keep one stadium per session initially.",
        f"Cuts: {len(summaries)}. Events: {sum(coverage.values())}. Interrupted/malformed lines: {malformed}.", "",
        "Automatic flags are suspect evidence, not proof that a shot is bad.",
        "The configured floor estimate is NOT measured stadium geometry. BallFollow mode is an existing heuristic, not a verified ball target.",
        "A pre-tick FOV may be contaminated by the preceding assist write; compare the post-tick cache and stereo input.", "",
        "Projection FOV ranges are symmetric approximations from m00; use the full matrix for asymmetric HMD frusta.", "",
        "## Highest Priority Cuts", "", "| Epoch/cut | Time (s) | Camera | Reasons |", "|---|---:|---|---|"]
    for cut in sorted(summaries, key=lambda cut: -cut["review_priority"])[:60]:
        report.append(f"| {cut['epoch']}/{cut['cut']} | {cut['start_seconds']:.2f}-{cut['end_seconds']:.2f} | "
            f"{', '.join(cut['ids'])} | {', '.join(cut['suspects']) or 'none'} |")
    report += ["", "## Implementation After Recording", "",
        "1. Separate shots using raw pose, dynamic FOV, play/replay state and stadium. Do not key only on the quantized camera ID.",
        "2. Review ambiguous learned neighbours and measured safety lifts; preserve known-good manual entries.",
        "3. Establish the game's real ball/subject provider. Follow the ball ONLY when the original game camera already follows it (hits/fouls), never by retargeting every cut.",
        "4. Derive dolly from verified subject distance and the actual projection. Clamp against verified local ground/dugout/stand geometry, not a stadium-wide guessed height.",
        "5. Shadow-evaluate proposed corrections, then A/B opt-in corrections with fallback to the working assist. candidate_review.json is not an importable runtime calibration.", "",
        "Files: camera_paths.csv, shot_catalogue.json, candidate_review.json, camera_paths.svg."]
    (output / "report.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    plot_paths(points, output / "camera_paths.svg")
    return catalogue_data


def plot_paths(points, path):
    all_points = [xy for pairs in points.values() for pair in pairs for xy in pair]
    if not all_points:
        path.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="960" height="720"><text x="30" y="40">No verified neutral camera observations</text></svg>', encoding="utf-8")
        return
    xs, ys = [v[0] for v in all_points], [v[1] for v in all_points]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    scale = min(860 / max(x1 - x0, 1), 590 / max(y1 - y0, 1))

    def xy(v):
        return f"{50+(v[0]-x0)*scale:.1f},{665-(v[1]-y0)*scale:.1f}"

    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="960" height="720" viewBox="0 0 960 720">',
           '<rect width="960" height="720" fill="#101f29"/>',
           '<text x="30" y="32" fill="#fff">Camera XY paths: game cache (grey), neutral before HMD (orange)</text>',
           '<text x="30" y="55" fill="#ccd">Camera trajectories only - not stadium geometry or ball positions</text>']
    for key, pairs in points.items():
        for index, color in ((0, "#697c88"), (1, "#edac57")):
            coordinates = " ".join(xy(pair[index]) for pair in pairs)
            svg.append(f'<polyline fill="none" stroke="{color}" stroke-width="1" points="{coordinates}"><title>Epoch/cut {key[0]}/{key[1]}</title></polyline>')
    svg.append("</svg>")
    path.write_text("\n".join(svg), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path, help="camera_traces/session-* directory")
    parser.add_argument("--output", type=Path, required=True, help="Separate review directory (not the game profile)")
    args = parser.parse_args()
    result = catalogue(args.session, args.output)
    print(f"Reviewed {len(result['cuts'])} cuts; output: {args.output}")

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prospi_analysis", ROOT / "tools/analyze_prospi_camera_trace.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AnalyzerTests(unittest.TestCase):
    def test_light_two_second_shots_keep_eye_context_and_marks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            session = root / "session"
            session.mkdir()
            (session / "metadata.json").write_text(json.dumps({"schema": 2, "session": 3,
                "sampling": "light", "limits": {"normal_hz": 1, "burst_hz": 0}}))
            events = []
            sequence = 0
            for shot in (1, 2, 3):
                pose = {"location": [shot * 2000, 0, 250], "rotation": [0, 0, 0], "fov": 20, "valid": True}
                assist = {"input": pose, "id": f"shot-{shot}"}
                for sample in range(2):
                    time = ((shot - 1) * 2 + sample + 1) * 1_000_000_000
                    sequence += 1
                    camera = sequence
                    base = {"time_ns": time, "epoch": 1, "cut_sequence": shot,
                            "camera_sequence": camera, "assist": assist}
                    events.append({**base, "kind": "camera", "sequence": sequence})
                    for eye in (0, 1):
                        sequence += 1
                        events.append({**base, "kind": "view", "sequence": sequence,
                            "view": {"input_matches_assist": True, "neutral_valid": True,
                                     "neutral": pose, "output": pose, "eye": eye}})
                        sequence += 1
                        events.append({**base, "kind": "projection", "sequence": sequence,
                            "projection": [1] + [0] * 15, "view": {"eye": eye}})
                sequence += 1
                events.append({**base, "kind": "marker", "sequence": sequence, "marker": 1,
                               "label": "quick manual mark", "time_ns": time + 500_000_000})
            (session / "events.jsonl").write_text("\n".join(json.dumps(e) for e in events))
            result = MODULE.catalogue(session, root / "review")
            self.assertEqual(len(result["cuts"]), 3)
            self.assertFalse(result["gaps"])
            self.assertEqual(result["malformed_lines"], 0)
            for cut in result["cuts"]:
                self.assertEqual(cut["views"], {"0": 2, "1": 2})
                self.assertEqual(len(cut["marks"]), 1)
                self.assertIsNotNone(cut["projection_symmetric_fov_range"])

    def test_native_target_is_not_ball_and_head_motion_is_separate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            session = root / "session"
            session.mkdir()
            (session / "metadata.json").write_text(json.dumps({"schema": 2, "session": 2}))
            pose = {"location": [0, 0, 0], "rotation": [0, 0, 0], "fov": 20, "valid": True}
            native = {"valid": True, "status": "accepted", "pose": pose, "focus_cm": 4000,
                      "look_at": [4000, 0, 0], "native_frame": 7, "object": 123}
            assist = {"input": pose, "native_source": native}
            events = [{"kind": "camera", "sequence": 1, "time_ns": 1000, "epoch": 1, "cut_sequence": 1,
                       "assist": assist},
                      {"kind": "view", "sequence": 2, "time_ns": 2000, "epoch": 1, "cut_sequence": 1,
                       "assist": assist, "view": {"source_matches_input": True, "input_matches_assist": True,
                           "neutral_valid": True, "neutral": pose, "output": pose, "eye": 0,
                           "target_framing": {"source": {"valid": True, "depth_cm": 4000},
                               "neutral": {"valid": True, "depth_cm": 3800, "behind": False},
                               "hmd": {"valid": True, "depth_cm": -100, "behind": True}}}},
                      {"kind": "observation", "time_ns": 3000, "epoch": 1, "cut_sequence": 1,
                       "observation": {"native_source": {"status": "changed_during_read", "valid": False}}}]
            (session / "events.jsonl").write_text("\n".join(json.dumps(e) for e in events))
            cut = MODULE.catalogue(session, root / "review")["cuts"][0]
            self.assertEqual(cut["targets_verified"], 0)
            self.assertEqual(cut["native_look_at_samples"], 1)
            self.assertEqual(cut["native_ranges"]["focus_cm"], [4000, 4000])
            self.assertEqual(cut["hmd_only_target_behind"], 1)
            self.assertEqual(cut["source_matched_views"], 1)
            self.assertEqual(cut["native_source_statuses"]["changed_during_read"], 1)
            self.assertNotIn("target_behind", cut["suspects"])

    def test_read_only_catalogue_and_incomplete_tail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            profile = root / "profile"
            session = profile / "camera_traces" / "session-1"
            session.mkdir(parents=True)
            (profile / "camera_calibration.json").write_text('{"version":1,"cameras":{}}')
            before = (profile / "camera_calibration.json").read_bytes()
            (session / "metadata.json").write_text(json.dumps({"schema": 1, "session": 1, "stadium": "test",
                "ball_follow_policy": "Only game-authored ball-follow cuts"}))
            pose = {"location": [1, 2, 3], "rotation": [0, 0, 0], "fov": 20, "valid": True}
            events = [{"kind": "camera", "sequence": 1, "time_ns": 1000, "epoch": 1, "cut_sequence": 1,
                "suspects": 6, "assist": {"input": pose, "id": "camera1", "play_mode": 4, "matches": [],
                    "focus_after": 1500, "previous_update_wrote_fov": True}},
                {"kind": "view", "sequence": 2, "time_ns": 2000, "epoch": 1, "cut_sequence": 1,
                    "assist": {"input": pose}, "view": {"neutral_valid": True, "neutral": pose, "output": pose, "eye": 0}},
                {"kind": "marker", "sequence": 3, "time_ns": 3000, "epoch": 1, "cut_sequence": 1, "marker": 1},
                {"kind": "gap", "time_ns": 4000, "label": "dropped samples"}]
            (session / "events.jsonl").write_text("\n".join(json.dumps(e) for e in events) + '\n{"incomplete":')
            result = MODULE.catalogue(session, root / "review")
            self.assertEqual(result["malformed_lines"], 1)
            self.assertEqual(len(result["cuts"]), 1)
            self.assertIn("large_safety_lift", result["cuts"][0]["suspects"])
            self.assertEqual(result["cuts"][0]["targets_verified"], 0)
            review = json.loads((root / "review/candidate_review.json").read_text())
            self.assertFalse(review["auto_apply"])
            self.assertFalse(review["calibration_import_compatible"])
            self.assertIsNone(review["review"][0]["proposed_correction"])
            self.assertEqual(before, (profile / "camera_calibration.json").read_bytes())
            with self.assertRaises(ValueError):
                MODULE.catalogue(session, profile)


if __name__ == "__main__":
    unittest.main()

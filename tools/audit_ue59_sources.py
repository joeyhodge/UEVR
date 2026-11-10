#!/usr/bin/env python3
"""Recheck the reviewed UE5.8.4 -> UE5.9 snapshot without building Unreal.

Reads licensed local source ZIPs; exports hashes, inventory and contract results,
not engine source. A new archive must be reviewed before changing the manifest.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile


def digest_file(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest().upper()


class Sources:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path)
        self.files = {}
        for entry in self.zip.infolist():
            if not entry.is_dir() and "/" in entry.filename:
                relative = entry.filename.split("/", 1)[1]
                if relative in self.files:
                    self.zip.close()
                    raise ValueError(f"duplicate archive path: {relative}")
                self.files[relative] = entry

    def read(self, path):
        return self.zip.read(self.files[path])

    def text(self, path):
        return self.read(path).decode("utf-8-sig")


def source_checks(old, new):
    results = []

    def check(name, predicate):
        results.append({"contract": name, "passed": bool(predicate)})

    engine = "Engine/Source/Runtime/"
    for path in ["Engine/Public/StereoRendering.h", "Engine/Public/StereoRenderTargetManager.h",
                 "Engine/Classes/Engine/LocalPlayer.h", "Engine/Private/LocalPlayer.cpp",
                 "Engine/Public/UnrealClient.h", "Engine/Public/Slate/SceneViewport.h",
                 "Engine/Private/Slate/SceneViewport.cpp", "Core/Public/HAL/IConsoleManager.h",
                 "CoreUObject/Public/UObject/ObjectMacros.h", "CoreUObject/Public/UObject/Field.h"]:
        check(f"unchanged shared contract: {path}", old.read(engine + path) == new.read(engine + path))
    player = new.text(engine + "Engine/Private/LocalPlayer.cpp")
    check("LocalPlayer histories allocate lazily rather than in PostInitProperties",
          "ViewStates[ViewIndex].Allocate(FeatureLevel)" in player
          and "ViewInitOptions.SceneViewStateInterface = ViewStates[ViewIndex].GetReference()" in player
          and "ULocalPlayer::PostInitProperties(" not in player)

    identifier = "HeadMountedDisplay/Public/IIdentifiableXRDevice.h"
    check("XR identifier gains an inherited virtual destructor",
          "virtual ~IXRSystemIdentifier()" not in old.text(engine + identifier)
          and "virtual ~IXRSystemIdentifier()" in new.text(engine + identifier))
    tracking = "HeadMountedDisplay/Public/IXRTrackingSystem.h"
    check("tracking methods unchanged; only inherited slot changes", old.read(engine + tracking) == new.read(engine + tracking))

    scene = "Engine/Public/SceneView.h"
    a, b = old.text(engine + scene), new.text(engine + scene)
    start = "class FSceneViewFamily\n"
    end = "class FSceneViewFamilyContext"
    a_family = a[a.index(start):a.index(end, a.index(start))]
    b_family = b[b.index(start):b.index(end, b.index(start))]
    check("complete FSceneViewFamily declaration unchanged", a_family == b_family)
    a_options = a[a.index("struct FSceneViewInitOptions :"):a.index("class FSceneView :")]
    b_options = b[b.index("struct FSceneViewInitOptions :"):b.index("class FSceneView :")]
    check("init-options explicitly gains ShaderPlatform", "EShaderPlatform ShaderPlatform;" not in a_options
          and "EShaderPlatform ShaderPlatform;" in b_options)

    slate = new.text(engine + "SlateRHIRenderer/Private/SlateRHIRenderer.cpp")
    prefix = slate[slate.index("struct FSlateDrawWindowPassInputs"):]
    prefix = prefix[:prefix.index("};")]
    fields = ["Renderer", "WindowElementList", "Window", "ViewportInfo", "Screenshot", "CursorPosition", "SceneViewRect", "ViewportScaleUI"]
    # Require actual declarations, not references in documentation or later code.
    declarations = [line.strip() for line in prefix.splitlines() if ";" in line]
    found = [next((i for i, line in enumerate(declarations) if re.search(r"\b" + field + r"\b", line)), -1) for field in fields]
    check("Slate screenshot snapshot precedes cursor/scene rect/scale", all(i >= 0 for i in found) and found == sorted(found))

    extension = new.text(engine + "Engine/Public/SceneViewExtension.h")
    check("new deferred translucency callback precedes postprocess", extension.index("virtual void PreRenderTranslucencyDeferred_RenderThread")
          < extension.index("virtual void PrePostProcessPass_RenderThread"))
    check("extension ABI must include the deprecated overload in MSVC's virtual group",
          len(re.findall(r"virtual void SubscribeToPostProcessingPass\(", extension)) == 2
          and extension.index("virtual bool IsActiveThisFrame(") < extension.rindex("virtual void SubscribeToPostProcessingPass("))
    check("development archive identifies as UE5.9.0", all(json.loads(new.text("Engine/Build/Build.version"))[k] == v
          for k, v in [("MajorVersion", 5), ("MinorVersion", 9), ("PatchVersion", 0)]))
    return results


def audit(baseline, candidate, manifest):
    old = Sources(baseline)
    try:
        new = Sources(candidate)
    except Exception:
        old.zip.close()
        raise
    try:
        common = old.files.keys() & new.files.keys()
        changed = sum((old.files[p].CRC, old.files[p].file_size) !=
                      (new.files[p].CRC, new.files[p].file_size) for p in common)
        report = {
            "schema": 1, "runtime_validated": False,
            "baseline_sha256": digest_file(baseline), "candidate_sha256": digest_file(candidate),
            "inventory_crc_size": {"modified": changed, "added": len(new.files.keys()-old.files.keys()),
                                   "removed": len(old.files.keys()-new.files.keys())},
            "critical_files": [], "errors": [],
        }
        for role in ["baseline", "candidate"]:
            if report[f"{role}_sha256"] != manifest[f"{role}_sha256"]:
                report["errors"].append(f"{role} archive differs from the reviewed snapshot")
        for expected in manifest["critical_files"]:
            path = expected["path"]
            entry = {"path": path}
            for label, sources in [("baseline", old), ("candidate", new)]:
                entry[f"{label}_sha256"] = hashlib.sha256(sources.read(path)).hexdigest() if path in sources.files else None
                if entry[f"{label}_sha256"] != expected[f"{label}_sha256"]:
                    report["errors"].append(f"{label} critical contract changed: {path}")
            entry["identical"] = entry["baseline_sha256"] == entry["candidate_sha256"]
            report["critical_files"].append(entry)
        report["checks"] = source_checks(old, new)
        for item in report["checks"]:
            if not item["passed"]:
                report["errors"].append(item["contract"])
        return report
    finally:
        old.zip.close()
        new.zip.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[1] / "docs/ue59-source-contracts.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = audit(args.baseline, args.candidate, json.loads(args.manifest.read_text(encoding="utf-8")))
    except (OSError, KeyError, ValueError, zipfile.BadZipFile) as exc:
        print(f"Source audit failed: {exc}", file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Reviewed {len(report['critical_files'])} critical files and {len(report['checks'])} source contracts; "
          f"errors={len(report['errors'])}; runtime validation pending")
    for error in report["errors"]:
        print(error, file=sys.stderr)
    return bool(report["errors"])


if __name__ == "__main__":
    sys.exit(main())

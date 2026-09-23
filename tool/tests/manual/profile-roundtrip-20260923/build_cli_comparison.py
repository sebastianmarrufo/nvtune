"""Build the manual live CLI comparison from captured nvtune JSON files."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
files = {
    "baseline": ("capture-original-profile.json", "capture-original-raw.json"),
    "modified": ("capture-modified-profile.json", "capture-modified-raw.json"),
    "restored": ("postdaemon-profile.json", "postdaemon-raw.json"),
}
data = {phase: (json.loads((root / profile).read_text(encoding="utf-8")),
                json.loads((root / raw).read_text(encoding="utf-8")))
        for phase, (profile, raw) in files.items()}
fields = {phase: pair[0]["fields"] for phase, pair in data.items()}
raw = {phase: pair[1]["registers"] for phase, pair in data.items()}
names = sorted(fields["baseline"])
scopes = ["broadcast"] + [f"fbpa{i}" for i in range(6)]
registers = ["CONFIG0", "CONFIG1", "CONFIG2", "CONFIG3", "CONFIG4",
             "CONFIG5", "TIMING22"]
assert all(set(fields[phase]) == set(names) for phase in files)
assert len(names) == 23
assert all(set(raw[phase]) == set(scopes) for phase in files)
assert all(set(raw[phase][scope]) == set(registers)
           for phase in files for scope in scopes)
assert fields["restored"] == fields["baseline"]
assert raw["restored"] == raw["baseline"]
field_changes = [n for n in names if fields["baseline"][n] != fields["modified"][n]]
assert field_changes == ["FAW", "RFC"], field_changes
assert [(fields["baseline"][n], fields["modified"][n]) for n in field_changes] == [(24, 25), (157, 158)]
assert fields["baseline"]["CL"] == fields["modified"]["CL"] == fields["restored"]["CL"] == 19
for scope in scopes:
    changes = [n for n in registers if raw["baseline"][scope][n] != raw["modified"][scope][n]]
    assert changes == ["CONFIG0", "CONFIG3"], (scope, changes)
    assert raw["modified"][scope]["CONFIG0"] == "0x16489E3A"
    assert raw["modified"][scope]["CONFIG3"] == "0x2200334A"

shots = ["01-cli-original.png", "02-cli-modified.png",
         "03-cli-pre-daemon-restored.png", "04-cli-daemon-running.png",
         "05-cli-daemon-restored.png"]
report = {
    "device": {"slot": "0000:01:00.0", "chip": "GP102 / TITAN Xp",
               "driver": "582.66", "pstate": "P2", "memory_clock_mhz_reported": 5508},
    "helper_sha256": "af4e5fb9d975509f908046ac37f47d1cc752c397362a6ab6fb9ac232dcda4e21",
    "screenshots_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                            for name in shots},
    "fields": {name: {phase: fields[phase][name] for phase in files} for name in names},
    "raw_registers": {scope: {name: {phase: raw[phase][scope][name] for phase in files}
                               for name in registers} for scope in scopes},
    "checks": {
        "exactly_two_fields_changed": True,
        "tcl_unchanged": True,
        "only_config0_config3_changed_in_each_scope": True,
        "final_49_raw_words_equal_baseline": True,
        "final_23_fields_equal_baseline": True,
        "daemon_ctrl_c_transcript_shows_restore": "restoring stock values...\n  0000:01:00.0: restored"
            in (root / "daemon-visible-transcript.txt").read_text(encoding="utf-8-sig"),
    },
}
assert all(report["checks"].values())
(root / "comparison-cli.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"checks": report["checks"], "scopes": len(scopes),
                  "fields": len(names), "screenshots": len(shots)}, indent=2))

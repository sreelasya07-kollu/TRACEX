# ruff: noqa: E501
"""Verdict table: every validator code -> plain-language message and what to do (Phase 7).

Policy (docs/phases/07): ERROR prompts a rescan, WARN proceeds with a note, INFO is shown only in
details. Upload is never blocked by the verdict itself (the server records it); the app decides what
to show. `docs/verdict-messages.md` is generated from this table (`ckit verdict-docs`) and a test keeps
it in sync with the spec and the validators.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass

RESCAN, PROCEED, NONE = "rescan", "proceed_with_note", "none"


@dataclass(frozen=True)
class Message:
    severity: str  # as raised by the validators
    level: str
    title: str  # <= 6 words, for a toast
    text: str  # what happened, plain language
    action: str  # what the user should do
    next: str  # RESCAN | PROCEED | NONE


V = Message
MESSAGES: dict[str, Message] = {
    "MISSING_MANIFEST": V("ERROR", "structural", "Scan is incomplete", "The scan did not finish saving.", "Scan the room again.", RESCAN),
    "ARCHIVE_UNREADABLE": V("ERROR", "structural", "Scan file is damaged", "The scan's packed file could not be opened safely.", "Scan the room again.", RESCAN),
    "MANIFEST_SCHEMA": V("ERROR", "structural", "Scan data is damaged", "The scan's summary file is not readable.", "Scan the room again.", RESCAN),
    "UNSUPPORTED_MAJOR": V("ERROR", "structural", "App update needed", "This scan was made by a newer version of the app.", "Update the app, then open the scan again.", RESCAN),
    "UNKNOWN_MINOR": V("WARN", "structural", "Newer scan format", "This scan uses newer, optional data we do not know yet.", "Nothing. Extra data is ignored.", PROCEED),
    "CONSENT_MISSING": V("ERROR", "structural", "Consent record missing", "The scan has no record that the person scanning accepted the capture terms.", "Accept the terms in the app, then scan the room again.", RESCAN),
    "REDACTION_MANIFEST_MISSING": V("ERROR", "structural", "Privacy record missing", "The scan has no record of how faces were blurred.", "Scan the room again.", RESCAN),
    "CAMERA_SCHEMA": V("ERROR", "structural", "Camera data is damaged", "The camera description is missing or invalid.", "Scan the room again.", RESCAN),
    "MISSING_FILE": V("ERROR", "structural", "Scan is missing files", "Some files of the scan are missing.", "Scan the room again.", RESCAN),
    "MCAP_TRUNCATED": V("ERROR", "structural", "Scan stopped early", "The motion recording ends abruptly.", "Scan the room again; keep the app open until it says done.", RESCAN),
    "MCAP_UNREADABLE": V("ERROR", "structural", "Scan data is damaged", "The motion recording cannot be read.", "Scan the room again.", RESCAN),
    "MISSING_CHANNEL": V("ERROR", "structural", "Sensor data missing", "A required sensor recording is missing (poses, motion or camera info).", "Scan again and allow camera and motion access.", RESCAN),
    "SHA256_MISMATCH": V("ERROR", "integrity", "Scan data changed", "A file does not match its checksum, so it was damaged or edited.", "Scan the room again.", RESCAN),
    "SIZE_MISMATCH": V("ERROR", "integrity", "Scan data changed", "A file has a different size than recorded.", "Scan the room again.", RESCAN),
    "REDACTION_ENTRY_BAD_PATH": V("WARN", "integrity", "Odd privacy log entry", "The privacy log names a file outside the photo folder; it is ignored.", "Nothing. It is ignored.", PROCEED),
    "REDACTION_ENTRY_NO_FILE": V("INFO", "integrity", "Blurred photo not saved", "The privacy log lists a photo that was not saved.", "Nothing needed.", NONE),
    "UNLISTED_FILE": V("WARN", "integrity", "Extra file in scan", "The scan contains a file that is not listed.", "Nothing. It is ignored.", PROCEED),
    "NON_MONOTONIC_TIME": V("ERROR", "semantic", "Timing error", "Timestamps go backwards, so frames cannot be matched to poses.", "Scan the room again.", RESCAN),
    "NAN_VALUE": V("ERROR", "semantic", "Invalid sensor values", "Some sensor readings are not valid numbers.", "Scan again; if it repeats, restart the phone.", RESCAN),
    "TIME_ZERO": V("ERROR", "semantic", "Timing error at the start", "The first camera position or camera info has no valid time, so frames cannot be matched to poses.", "Scan the room again; if it repeats, restart the app.", RESCAN),
    "NON_UNIT_QUATERNION": V("ERROR", "semantic", "Invalid camera rotations", "Some camera rotations are not valid.", "Scan the room again.", RESCAN),
    "MISSING_INTRINSICS": V("ERROR", "semantic", "Camera info missing", "Camera lens information is missing for the first photos.", "Scan the room again.", RESCAN),
    "KEYFRAME_WITHOUT_POSE": V("ERROR", "semantic", "Photos without position", "Some photos have no tracked camera position.", "Scan again, moving slowly while tracking is good.", RESCAN),
    "KEYFRAME_FILE_MISSING": V("ERROR", "semantic", "Photos are missing", "The scan lists photos that were not saved.", "Scan the room again; make sure storage is not full.", RESCAN),
    "COUNT_MISMATCH": V("WARN", "semantic", "Counts differ", "Summary counts differ slightly from the recording.", "Nothing. The recording is used as is.", PROCEED),
    "KEYFRAME_TRACKING_LIMITED": V("WARN", "semantic", "Shaky tracking in places", "Some photos were taken while tracking was weak.", "Optional: rescan those areas slowly with more light.", PROCEED),
    "POSE_GAP": V("WARN", "semantic", "Tracking was lost briefly", "Tracking was interrupted for part of the scan.", "Optional: rescan the area where tracking was lost.", PROCEED),
    "NO_KEYFRAMES": V("ERROR", "semantic", "No photos were saved", "The scan contains no usable photos, so no 3D result can be made.", "Scan the room again; walk slowly and keep the app open until it says done.", RESCAN),
    "FEW_KEYFRAMES": V("WARN", "semantic", "Not many photos", "The scan has few usable photos, so the 3D result may have gaps.", "Walk the room again, slower, covering all walls.", PROCEED),
    "NO_AR_GEOMETRY": V("INFO", "semantic", "No surface hints", "The phone did not report surfaces or feature points.", "Nothing needed.", NONE),
    "DETECTION_BAD_BOX": V("WARN", "semantic", "Odd object boxes", "Some detected object boxes are not valid.", "Nothing. They are ignored.", PROCEED),
    "DETECTION_UNKNOWN_FRAME": V("INFO", "semantic", "Detections without photo", "Some object detections refer to photos that are not in the scan.", "Nothing needed.", NONE),
    "GUIDE_OUTPUT_BAD_VALUE": V("WARN", "semantic", "Odd room labels", "Some room outline, object or gap data in the scan is not valid.", "Nothing. It is ignored.", PROCEED),
    "LABEL_WALL_UNKNOWN": V("INFO", "semantic", "Door or window without wall", "A door or window refers to a wall that is not in the room outline.", "Nothing needed.", NONE),
    "PARTIAL_BUNDLE": V("WARN", "semantic", "Scan was cut short", "The scan was interrupted and recovered, so part of the room may be missing.", "Scan the missing part again.", PROCEED),
    "QUALITY_NOT_EVALUATED": V("INFO", "quality", "Quality not checked", "Quality metrics were not available for this scan.", "Nothing needed.", NONE),
}  # fmt: skip

_ORDER = {"ERROR": 0, "WARN": 1, "INFO": 2}
HEADLINES = {
    "FAIL": "This scan cannot be used. Please scan the room again.",
    "WARN": "Scan saved with notes.",
    "PASS": "Scan looks good.",
}


def for_report(issues: list[dict]) -> dict:
    """issues: validator issues ({code, severity, ...}). Returns the user-facing verdict."""
    known = [(i, MESSAGES.get(i["code"])) for i in issues]
    rows = sorted(known, key=lambda x: (_ORDER.get(x[0]["severity"], 3), x[0]["code"]))
    items = [
        {
            "code": i["code"],
            "severity": i["severity"],
            "title": m.title if m else i["code"],
            "text": m.text if m else i.get("message", ""),
            "action": m.action if m else "",
            "next": m.next if m else NONE,
            "detail": i.get("message", ""),
        }
        for i, m in rows
    ]
    verdict = "FAIL" if any(i["severity"] == "ERROR" for i, _ in rows) else (
        "WARN" if any(i["severity"] == "WARN" for i, _ in rows) else "PASS"
    )  # fmt: skip
    head = HEADLINES[verdict]
    return {"verdict": verdict, "headline": head, "rescan": verdict == "FAIL", "items": items}


def to_markdown() -> str:
    out = [
        "# Verdict messages",
        "",
        "Generated from `tools/ckit/verdict.py` (`ckit verdict-docs`). Do not edit by hand.",
        "ERROR -> rescan. WARN -> proceed with a note. INFO -> details only.",
        "",
        "| Code | Level | Severity | Title | What happened | What to do | Next |",
        "|---|---|---|---|---|---|---|",
    ]
    for code, m in MESSAGES.items():
        out.append(
            f"| {code} | {m.level} | {m.severity} | {m.title} | {m.text} | {m.action} | {m.next} |"
        )
    return "\n".join(out) + "\n"


def to_json() -> str:
    """Android asset (`ckit verdict-export`): {"headlines": {...}, "codes": {CODE: message fields}}."""
    doc = {"headlines": HEADLINES, "codes": {c: asdict(m) for c, m in MESSAGES.items()}}
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"

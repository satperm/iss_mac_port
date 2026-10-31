#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline bounded perf captures. No game data or external dependencies.
Usage: python scripts/perf_trace_report.py capture.jsonl --out report
A capture is a diagnostic sample, never a whole-session benchmark.
"""

import argparse
import bisect
from collections import Counter
import html
import json
import math
from pathlib import Path

KINDS = {
    1: "frame",
    2: "marker",
    3: "package_open",
    4: "pipeline_request",
    5: "compile_begin",
    6: "compile_end",
    7: "wait_begin",
    8: "wait_end",
    9: "resource_create",
    10: "resource_evict",
    11: "resource_upload",
    12: "fault_begin",
    13: "fault_end",
    14: "shader_load",
    15: "shader_translate",
    16: "memory_pressure",
    17: "status",
    18: "pipeline_feedback",
    19: "queue_enqueued",
    20: "queue_begin",
    21: "queue_end",
    22: "resource_read",
    23: "readback_apply",
    24: "fault_context",
}
MARKERS = {1: "absorb_smoke", 2: "use_power", 3: "movement", 4: "other"}


def read_trace(path):
    warnings = []
    events = []
    header = None
    footer = None
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line_no, line in enumerate(stream, 1):
            if len(line) > 8192:
                warnings.append("oversized_record")
                continue
            try:
                item = json.loads(line)
            except (ValueError, TypeError):
                warnings.append("invalid_or_truncated_record")
                continue
            if not isinstance(item, dict):
                warnings.append("invalid_record")
                continue
            if "session" in item:
                if header is not None:
                    warnings.append("multiple_sessions")
                    break
                header = item
                if item.get("schema") != 1:
                    return dict(
                        events=[],
                        header=item,
                        footer=None,
                        warnings=["unsupported_schema"],
                    )
            elif "capture_end" in item:
                footer = item
            elif header is None:
                warnings.append("missing_header")
            elif all(
                isinstance(item.get(k), int)
                and not isinstance(item[k], bool)
                and item[k] >= 0
                for k in (
                    "kind",
                    "ts_ns",
                    "frame",
                    "op",
                    "object",
                    "generation",
                    "a",
                    "b",
                    "thread",
                    "role",
                )
            ):
                if item["kind"] not in KINDS:
                    warnings.append("unknown_event_kind")
                else:
                    events.append(item)
            else:
                warnings.append("invalid_event")
    if header is None:
        warnings.append("missing_header")
    if footer is None:
        warnings.append("missing_footer")
    else:
        if footer.get("dropped_total", 0):
            warnings.append("events_lost_in_session")
        if footer.get("truncated"):
            warnings.append("disk_limit_reached")
        if header and footer.get("first_ns", 0) > max(
            0, header.get("trigger_ns", 0) - header.get("pre_requested_ns", 0)
        ):
            warnings.append("pre_window_incomplete")
    return dict(
        events=events, header=header, footer=footer, warnings=sorted(set(warnings))
    )


def union(intervals):
    total = 0
    end = None
    for a, b in sorted(intervals):
        total += b - a if end is None or a >= end else max(0, b - end)
        end = b if end is None else max(end, b)
    return total


def analyze(events):
    spans = []
    incomplete = []
    pending = {}
    # Sort only after pairing: an end timestamp earlier than its begin is invalid evidence.
    for e in events:
        k = e["kind"]
        key = (e["thread"], e["op"])
        if k in (5, 7, 12, 20):
            if key in pending:
                incomplete.append(pending[key])
            pending[key] = e
        elif k in (6, 8, 13, 21):
            begin = pending.pop(key, None)
            if begin is None or begin["kind"] + 1 != k or e["ts_ns"] < begin["ts_ns"]:
                incomplete.append(e)
                if begin is not None:  # popped above: keep it visible, not silently lost
                    incomplete.append(begin)
                continue
            spans.append(
                dict(
                    category={6: "compile", 8: "wait", 13: "fault", 21: "queue_work"}[
                        k
                    ],
                    start_ns=begin["ts_ns"],
                    end_ns=e["ts_ns"],
                    duration_ns=e["ts_ns"] - begin["ts_ns"],
                    op=e["op"],
                    thread=e["thread"],
                    object=begin["object"],
                    generation=begin["generation"],
                    parent_op=begin["b"],
                )
            )
    incomplete.extend(pending.values())
    ordered = sorted(events, key=lambda x: x["ts_ns"])
    frames = [e for e in ordered if e["kind"] == 1 and e["a"] > 0]
    markers = [
        dict(
            ts_ns=e["ts_ns"], frame=e["frame"], category=MARKERS.get(e["a"], "unknown")
        )
        for e in ordered
        if e["kind"] == 2
    ]
    removed = {}
    recreations = 0
    for e in ordered:
        if e["kind"] == 10:
            removed[e["object"]] = e["generation"]
        if (
            e["kind"] == 9
            and e["object"] in removed
            and removed[e["object"]] != e["generation"]
        ):
            recreations += 1
            removed.pop(e["object"])
    timestamps = [e["ts_ns"] for e in ordered]
    stalls = []
    for f in frames:
        end = f["ts_ns"]
        start = max(0, end - f["a"])
        if f["a"] < 100_000_000:
            continue
        overlap = [s for s in spans if s["start_ns"] < end and s["end_ns"] > start]
        nearby = [m for m in markers if abs(m["ts_ns"] - end) <= 5_000_000_000]
        left = bisect.bisect_left(timestamps, start)
        right = bisect.bisect_right(timestamps, end)
        evidence = [
            e
            for e in ordered[left:right]
            if e["kind"] not in (1, 5, 6, 7, 8, 12, 13, 20, 21)
        ]
        stalls.append(
            dict(
                events=evidence[:512],
                evidence_events_truncated=len(evidence) > 512,
                frame=f["frame"],
                duration_ms=f["a"] / 1e6,
                start_ns=start,
                end_ns=end,
                evidence="temporal_association_only",
                markers=nearby,
                spans=overlap,
            )
        )
    values = sorted(e["a"] / 1e6 for e in frames)
    limits = [
        "capture_only_not_session_benchmark",
        "time_overlap_does_not_prove_cause",
        "same_origin_is_not_content_identity",
        "driver_cache_unknown_without_valid_feedback",
    ]
    if not frames:
        limits.append("no_frame_data")
    if incomplete:
        limits.append("unpaired_spans")
    feedback = [
        dict(
            op=e["op"],
            object=e["object"],
            pipeline_kind=chr(e["generation"])
            if e["generation"] in (99, 103)
            else "unknown",
            driver_duration_ns=e["a"] if e["b"] in (1, 2) else None,
            cache={
                0: "unknown",
                1: "application_hit",
                2: "valid_without_application_hit",
            }.get(e["b"], "unknown"),
        )
        for e in ordered
        if e["kind"] == 18
    ]
    requests = [
        dict(
            ts_ns=e["ts_ns"],
            object=e["object"],
            pipeline_kind=chr(e["a"]) if e["a"] in (99, 103) else "unknown",
            state={
                0: "absent_ram_downstream_unknown",
                1: "compile_pending",
                2: "lazy_programs_mode",
            }.get(e["b"], "unknown"),
        )
        for e in ordered
        if e["kind"] == 4
    ]
    submitted = {e["op"]: e for e in ordered if e["kind"] == 19}
    queues = []
    for e in ordered:
        if e["kind"] != 20:
            continue
        before = submitted.get(e["op"])
        queues.append(
            dict(
                op=e["op"],
                queue=e["object"],
                parent_op=before["b"] if before else None,
                wait_ns=e["ts_ns"] - before["ts_ns"]
                if before and e["ts_ns"] >= before["ts_ns"]
                else None,
            )
        )
    requested_reads = {e["op"]: e for e in ordered if e["kind"] == 22}
    buffer_reads = []
    for e in ordered:
        if e["kind"] != 23:
            continue
        request = requested_reads.get(e["op"])
        if request is not None and request["ts_ns"] <= e["ts_ns"]:
            buffer_reads.append(
                dict(
                    op=e["op"],
                    object=e["object"],
                    bytes=e["a"],
                    request_to_apply_start_ns=e["ts_ns"] - request["ts_ns"],
                )
            )
    return dict(
        buffer_reads=buffer_reads,
        queues=queues,
        pipeline_requests=requests,
        pipeline_feedback=feedback,
        event_counts=dict(Counter(KINDS.get(e["kind"], "unknown") for e in events)),
        markers=markers,
        spans=spans,
        incomplete_spans=incomplete,
        stalls=stalls,
        span_union_ns={
            name: union(
                (s["start_ns"], s["end_ns"]) for s in spans if s["category"] == name
            )
            for name in ("compile", "wait", "fault", "queue_work")
        },
        same_origin_recreations=recreations,
        proven_texture_churn=False,
        capture_frame_p99_ms=values[max(0, math.ceil(0.99 * len(values)) - 1)]
        if values
        else None,
        limitations=limits,
    )


def read_frames(path, start_frame=1, end_frame=None):
    rows = []
    warnings = []
    closed = False
    with Path(path).open(encoding="utf-8") as stream:
        if stream.readline().strip() != "frame,ts_ns,interval_ns":
            return dict(
                complete=False,
                warnings=["unsupported_frame_schema"],
                fps=None,
                p99_ms=None,
            )
        for line in stream:
            if line.startswith("# end,"):
                closed = True
                if "dropped_total=0," not in line:
                    warnings.append("events_lost_in_session")
                if "truncated=0" not in line:
                    warnings.append("frame_size_limit_reached")
                continue
            try:
                row = tuple(map(int, line.strip().split(",")))
                if len(row) != 3 or min(row) < 0:
                    raise ValueError()
                rows.append(row)
            except ValueError:
                warnings.append("invalid_frame_record")
    if not closed:
        warnings.append("missing_frame_footer")
    chosen = [
        r
        for r in rows
        if r[0] >= start_frame and (end_frame is None or r[0] <= end_frame)
    ]
    if not chosen:
        warnings.append("no_frame_data")
    if chosen and chosen[0][0] != start_frame:
        warnings.append("missing_start_frame")
    if chosen and end_frame is not None and chosen[-1][0] != end_frame:
        warnings.append("missing_end_frame")
    if any(b[0] != a[0] + 1 or b[1] < a[1] for a, b in zip(chosen, chosen[1:])):
        warnings.append("frame_gap_or_order_error")
    values = sorted(r[2] for r in chosen if r[2] > 0)
    return dict(
        complete=not warnings,
        warnings=sorted(set(warnings)),
        frames=len(values),
        scope="selected_frame_range"
        if end_frame is not None or start_frame != 1
        else "entire_recorded_session",
        fps=len(values) * 1e9 / sum(values) if values and sum(values) else None,
        p99_ms=values[max(0, math.ceil(0.99 * len(values)) - 1)] / 1e6
        if values
        else None,
        percentile_definition="nearest_rank_of_nonzero_intervals",
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, nargs="?")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--frames", type=Path)
    parser.add_argument("--start-frame", type=int, default=1)
    parser.add_argument("--end-frame", type=int)
    args = parser.parse_args(argv)
    if not args.capture and not args.frames:
        parser.error("supply a capture or --frames")
    data = (
        read_trace(args.capture)
        if args.capture
        else dict(events=[], header=None, footer=None, warnings=["frame_series_only"])
    )
    result = analyze(data.pop("events"))
    result.update(data)
    if args.frames:
        result["frame_series"] = read_frames(
            args.frames, args.start_frame, args.end_frame
        )
        # The console line and the HTML read result["warnings"] only: an incomplete frame
        # series must show there too, not just inside frame_series.
        result["warnings"] = sorted(
            set(result["warnings"])
            | {"frames:" + w for w in result["frame_series"]["warnings"]}
        )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.with_suffix(".json").write_text(
        json.dumps(result, indent=2), encoding="utf-8"
    )
    body = "<h1>Performance capture</h1><p>Captured intervals only. Association is not proof of cause.</p>"
    body += (
        "<p>Coverage warnings: "
        + html.escape(", ".join(result["warnings"]) or "none detected")
        + "</p>"
    )
    for i, s in enumerate(result["stalls"], 1):
        body += (
            f"<details><summary>Frame {s['frame']}: {s['duration_ms']:.2f} ms</summary><pre>"
            + html.escape(json.dumps(s, indent=2))
            + "</pre></details>"
        )
    body += (
        "<details><summary>Full evidence and limitations</summary><pre>"
        + html.escape(json.dumps(result, indent=2))
        + "</pre></details>"
    )
    args.out.with_suffix(".html").write_text(
        '<!doctype html><meta charset="utf-8"><title>Performance capture</title><style>body{font:16px system-ui;max-width:1000px;margin:2rem auto;padding:1rem}pre{white-space:pre-wrap}details{padding:1rem;border-bottom:1px solid #bbb}</style>'
        + body,
        encoding="utf-8",
    )
    print(f"{len(result['stalls'])} captured stalls; warnings: {result['warnings']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

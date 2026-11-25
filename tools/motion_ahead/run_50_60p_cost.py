#!/usr/bin/env python3
"""Z4 / S1 50p & 60p motion-estimate-ahead-spare A/B cost line runner and ledger parser.

Rule 1: frame budgets are derived from nominal frame rate (1000/50 = 20.0 ms, 1000/60 = 16.667 ms).
Contract (LaneAheadPolicy.h / Z4):
  - Baseline:  motion_estimate_ahead_spare=off
  - Candidate: motion_estimate_ahead_spare=on
  - Acceptance gate: render_ms_saved > 0.0 AND added_late_presents == 0 for BOTH 50p and 60p.
  - DEFAULT_FLIP_ALLOWED is strictly False: this tool reports a recommendation only and NEVER
    edits any configuration file, default table, or C++ header.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

# Physics-derived frame budgets (ms).
RATE_50P_HZ: float = 50.0
RATE_60P_HZ: float = 60.0
BUDGET_50P_MS: float = 1000.0 / RATE_50P_HZ
BUDGET_60P_MS: float = 1000.0 / RATE_60P_HZ

RATES: Dict[str, Tuple[float, float]] = {
    "50p": (RATE_50P_HZ, BUDGET_50P_MS),
    "60p": (RATE_60P_HZ, BUDGET_60P_MS),
}

# Explicit contract guard: Z4 runner NEVER flips motion_estimate_ahead_spare default.
DEFAULT_FLIP_ALLOWED: bool = False

_RENDER_MS_PATTERNS: Tuple[re.Pattern[str], ...] = (
    re.compile(r"\brender(?:_time)?_ms\s*[:=]\s*([0-9]+(?:\.[0-9]+)?)", re.IGNORECASE),
    re.compile(r"\brenderMs\s*[:=]\s*([0-9]+(?:\.[0-9]+)?)", re.IGNORECASE),
    re.compile(r"\brender(?:\s+time)?\s*[:=]\s*([0-9]+(?:\.[0-9]+)?)\s*ms\b", re.IGNORECASE),
    re.compile(r"\bframe\s+render\s+([0-9]+(?:\.[0-9]+)?)\s*ms\b", re.IGNORECASE),
)

_LATE_PRESENTS_PATTERNS: Tuple[re.Pattern[str], ...] = (
    re.compile(r"\blate_presents\s*[:=]\s*([0-9]+)", re.IGNORECASE),
    re.compile(r"\blate\s+presents\s*[:=]\s*([0-9]+)", re.IGNORECASE),
    re.compile(r"\blate(?:_?drops)\s*[:=]\s*([0-9]+)", re.IGNORECASE),
    re.compile(r"\blate\s*[:=]\s*([0-9]+)", re.IGNORECASE),
)

_AHEAD_LINE_RE = re.compile(r"motion\s+field\s+ahead\s*\(([^)]*)\)", re.IGNORECASE)
_AHEAD_SLOTS_PATTERNS: Tuple[re.Pattern[str], ...] = (
    re.compile(r"\bslots?\s*[:=]\s*([0-9]+)", re.IGNORECASE),
    re.compile(r"\b([0-9]+)\s+slots?\b", re.IGNORECASE),
    re.compile(r"\bhits?\s*[:=]\s*([0-9]+)", re.IGNORECASE),
)
_AHEAD_RENDERS_PATTERNS: Tuple[re.Pattern[str], ...] = (
    re.compile(r"\brenders?\s*[:=]\s*([0-9]+)", re.IGNORECASE),
    re.compile(r"\b([0-9]+)\s+renders?\b", re.IGNORECASE),
    re.compile(r"\bsequential\s*[:=]\s*([0-9]+)", re.IGNORECASE),
)


@dataclass
class RunMetrics:
    source: str
    sample_count: int
    render_ms_mean: float
    render_ms_p95: float
    late_presents: int
    ahead_slots: int = 0
    ahead_renders: int = 0
    ahead_lines: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "source": self.source,
            "sample_count": self.sample_count,
            "render_ms_mean": round(self.render_ms_mean, 6),
            "render_ms_p95": round(self.render_ms_p95, 6),
            "late_presents": self.late_presents,
            "motion_field_ahead": {
                "slots": self.ahead_slots,
                "renders": self.ahead_renders,
                "lines_found": len(self.ahead_lines),
            },
        }


@dataclass
class RateEvaluation:
    rate: str
    hz: float
    budget_ms: float
    baseline: RunMetrics
    candidate: RunMetrics
    render_ms_saved: float
    p95_ms_saved: float
    delta_late_presents: int
    added_late_presents: int
    passed: bool
    reasons: List[str]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "rate": self.rate,
            "hz": self.hz,
            "budget_ms": round(self.budget_ms, 6),
            "baseline": self.baseline.to_dict(),
            "candidate": self.candidate.to_dict(),
            "render_ms_saved": round(self.render_ms_saved, 6),
            "p95_ms_saved": round(self.p95_ms_saved, 6),
            "delta_late_presents": self.delta_late_presents,
            "added_late_presents": self.added_late_presents,
            "passed": self.passed,
            "reasons": list(self.reasons),
        }


def compute_mean(samples: Sequence[float]) -> float:
    if not samples:
        return 0.0
    return sum(samples) / float(len(samples))


def compute_p95(samples: Sequence[float]) -> float:
    if not samples:
        return 0.0
    ordered = sorted(samples)
    # Deterministic nearest-rank 95th percentile: ceil(0.95 * N) - 1, clamped to [0, N-1].
    idx = int(math.ceil(0.95 * len(ordered))) - 1
    idx = max(0, min(len(ordered) - 1, idx))
    return ordered[idx]


def _extract_first_int(text: str, patterns: Sequence[re.Pattern[str]]) -> Optional[int]:
    for pat in patterns:
        m = pat.search(text)
        if m:
            return int(m.group(1))
    return None


def parse_log_text(text: str, source: str = "<log>") -> RunMetrics:
    render_samples: List[float] = []
    late_values: List[int] = []
    ahead_lines: List[str] = []
    ahead_slots = 0
    ahead_renders = 0

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue

        for pat in _RENDER_MS_PATTERNS:
            matches = pat.findall(line)
            if matches:
                for val in matches:
                    fval = float(val)
                    if math.isfinite(fval):
                        render_samples.append(fval)
                break

        late_val = _extract_first_int(line, _LATE_PRESENTS_PATTERNS)
        if late_val is not None:
            late_values.append(late_val)

        m_ahead = _AHEAD_LINE_RE.search(line)
        if m_ahead:
            ahead_lines.append(line)
            inside = m_ahead.group(1)
            s_val = _extract_first_int(inside, _AHEAD_SLOTS_PATTERNS)
            r_val = _extract_first_int(inside, _AHEAD_RENDERS_PATTERNS)
            if s_val is not None:
                ahead_slots = max(ahead_slots, s_val)
            if r_val is not None:
                ahead_renders = max(ahead_renders, r_val)

    late_presents = max(late_values) if late_values else 0
    return RunMetrics(
        source=source,
        sample_count=len(render_samples),
        render_ms_mean=compute_mean(render_samples),
        render_ms_p95=compute_p95(render_samples),
        late_presents=late_presents,
        ahead_slots=ahead_slots,
        ahead_renders=ahead_renders,
        ahead_lines=ahead_lines,
    )


def parse_csv_text(text: str, source: str = "<csv>") -> RunMetrics:
    reader = csv.DictReader(io.StringIO(text))
    render_samples: List[float] = []
    late_flags_sum = 0
    late_cumulative_max: Optional[int] = None
    ahead_slots = 0
    ahead_renders = 0
    ahead_lines: List[str] = []

    for row in reader:
        norm = {
            (k.strip().lower() if k is not None else ""): (v.strip() if v is not None else "")
            for k, v in row.items()
        }
        for key in ("render_ms", "render_time_ms", "renderms", "render"):
            if key in norm and norm[key] != "":
                val = float(norm[key])
                if math.isfinite(val):
                    render_samples.append(val)
                break

        if "late_presents" in norm and norm["late_presents"] != "":
            cur = int(float(norm["late_presents"]))
            late_cumulative_max = cur if late_cumulative_max is None else max(late_cumulative_max, cur)
        elif "late" in norm and norm["late"] != "":
            late_flags_sum += int(float(norm["late"]))
        elif "is_late" in norm and norm["is_late"] != "":
            late_flags_sum += int(float(norm["is_late"]))

        for key in ("ahead_slots", "motion_field_ahead_slots", "slots"):
            if key in norm and norm[key] != "":
                ahead_slots = max(ahead_slots, int(float(norm[key])))
                break
        for key in ("ahead_renders", "motion_field_ahead_renders", "renders"):
            if key in norm and norm[key] != "":
                ahead_renders = max(ahead_renders, int(float(norm[key])))
                break

        note = norm.get("motion_field_ahead", "") or norm.get("status", "")
        if note and _AHEAD_LINE_RE.search(note):
            ahead_lines.append(note)

    late_presents = late_cumulative_max if late_cumulative_max is not None else late_flags_sum
    return RunMetrics(
        source=source,
        sample_count=len(render_samples),
        render_ms_mean=compute_mean(render_samples),
        render_ms_p95=compute_p95(render_samples),
        late_presents=late_presents,
        ahead_slots=ahead_slots,
        ahead_renders=ahead_renders,
        ahead_lines=ahead_lines,
    )


def parse_json_data(data: Mapping[str, Any], source: str = "<json>") -> RunMetrics:
    raw_samples = data.get("render_ms_samples") or data.get("render_ms")
    samples: List[float] = []
    if isinstance(raw_samples, list):
        for item in raw_samples:
            fval = float(item)
            if math.isfinite(fval):
                samples.append(fval)

    if samples:
        mean_ms = compute_mean(samples)
        p95_ms = compute_p95(samples)
        sample_count = len(samples)
    else:
        mean_ms = float(data.get("render_ms_mean", 0.0))
        p95_ms = float(data.get("render_ms_p95", mean_ms))
        sample_count = int(data.get("sample_count", 1 if mean_ms > 0.0 else 0))

    late_presents = int(data.get("late_presents", 0))
    ahead_obj = data.get("motion_field_ahead")
    ahead_slots = 0
    ahead_renders = 0
    ahead_lines: List[str] = []

    if isinstance(ahead_obj, Mapping):
        ahead_slots = int(ahead_obj.get("slots", 0))
        ahead_renders = int(ahead_obj.get("renders", 0))
    else:
        ahead_slots = int(data.get("ahead_slots", 0))
        ahead_renders = int(data.get("ahead_renders", 0))

    raw_lines = data.get("motion_field_ahead_lines")
    if isinstance(raw_lines, list):
        for line in raw_lines:
            s = str(line)
            ahead_lines.append(s)
            m = _AHEAD_LINE_RE.search(s)
            if m:
                s_val = _extract_first_int(m.group(1), _AHEAD_SLOTS_PATTERNS)
                r_val = _extract_first_int(m.group(1), _AHEAD_RENDERS_PATTERNS)
                if s_val is not None:
                    ahead_slots = max(ahead_slots, s_val)
                if r_val is not None:
                    ahead_renders = max(ahead_renders, r_val)

    return RunMetrics(
        source=source,
        sample_count=sample_count,
        render_ms_mean=mean_ms,
        render_ms_p95=p95_ms,
        late_presents=late_presents,
        ahead_slots=ahead_slots,
        ahead_renders=ahead_renders,
        ahead_lines=ahead_lines,
    )


def parse_run_file(path: Path, input_format: str = "auto") -> RunMetrics:
    text = path.read_text(encoding="utf-8", errors="replace")
    fmt = input_format.lower()
    if fmt == "auto":
        ext = path.suffix.lower()
        if ext == ".json":
            fmt = "json"
        elif ext == ".csv":
            fmt = "csv"
        else:
            fmt = "log"

    if fmt == "json":
        obj = json.loads(text)
        if not isinstance(obj, Mapping):
            raise ValueError(f"Expected JSON object in {path}")
        return parse_json_data(obj, source=str(path))
    if fmt == "csv":
        return parse_csv_text(text, source=str(path))
    return parse_log_text(text, source=str(path))


def evaluate_rate(rate: str, baseline: RunMetrics, candidate: RunMetrics) -> RateEvaluation:
    if rate not in RATES:
        raise ValueError(f"Unsupported rate '{rate}'; expected one of {sorted(RATES)}")
    hz, budget_ms = RATES[rate]
    render_ms_saved = baseline.render_ms_mean - candidate.render_ms_mean
    p95_ms_saved = baseline.render_ms_p95 - candidate.render_ms_p95
    delta_late = candidate.late_presents - baseline.late_presents
    added_late = max(0, delta_late)

    reasons: List[str] = []
    if baseline.sample_count <= 0:
        reasons.append("baseline has no render_ms samples")
    if candidate.sample_count <= 0:
        reasons.append("candidate has no render_ms samples")
    if not (render_ms_saved > 0.0):
        reasons.append(f"render_ms_saved ({render_ms_saved:+.3f} ms) <= 0.0")
    if added_late != 0:
        reasons.append(f"added_late_presents ({added_late}) != 0")

    return RateEvaluation(
        rate=rate,
        hz=hz,
        budget_ms=budget_ms,
        baseline=baseline,
        candidate=candidate,
        render_ms_saved=render_ms_saved,
        p95_ms_saved=p95_ms_saved,
        delta_late_presents=delta_late,
        added_late_presents=added_late,
        passed=(len(reasons) == 0),
        reasons=reasons,
    )


def evaluate_ab(rate_evals: Mapping[str, RateEvaluation]) -> Dict[str, Any]:
    missing_rates = [r for r in ("50p", "60p") if r not in rate_evals]
    both_present = len(missing_rates) == 0
    overall_passed = both_present and all(rate_evals[r].passed for r in ("50p", "60p"))
    recommendation = "RECORD_S1_ACCEPTANCE_ROW" if overall_passed else "KEEP_AUTO_OFF"

    return {
        "gate": "Z4_S1_50P_60P_COST_LINE",
        "default_flip_allowed": DEFAULT_FLIP_ALLOWED,
        "overall_passed": overall_passed,
        "missing_rates": missing_rates,
        "recommendation": recommendation,
        "criteria": {
            "render_ms_saved_gt": 0.0,
            "added_late_presents_eq": 0,
            "required_rates": ["50p", "60p"],
        },
        "rates": {k: rate_evals[k].to_dict() for k in sorted(rate_evals.keys())},
    }


def format_markdown_ledger(summary: Mapping[str, Any]) -> str:
    lines = [
        "| Rate | Budget (ms) | Baseline Mean / p95 (ms) | Candidate Mean / p95 (ms) | Saved Mean / p95 (ms) | Late (Base -> Cand, Added) | Ahead (Slots / Renders) | Gate |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | :---: |",
    ]
    rates = summary.get("rates", {})
    for rate in ("50p", "60p"):
        if rate not in rates:
            hz, budget = RATES[rate]
            lines.append(
                f"| {rate} ({hz:.0f} Hz) | {budget:.3f} | missing | missing | n/a | n/a | n/a | MISSING |"
            )
            continue
        r = rates[rate]
        b = r["baseline"]
        c = r["candidate"]
        ca = c["motion_field_ahead"]
        verdict = "PASS" if r["passed"] else "FAIL"
        lines.append(
            f"| {rate} | {r['budget_ms']:.3f} | "
            f"{b['render_ms_mean']:.3f} / {b['render_ms_p95']:.3f} | "
            f"{c['render_ms_mean']:.3f} / {c['render_ms_p95']:.3f} | "
            f"{r['render_ms_saved']:+.3f} / {r['p95_ms_saved']:+.3f} | "
            f"{b['late_presents']} -> {c['late_presents']} (+{r['added_late_presents']}) | "
            f"{ca['slots']} / {ca['renders']} | {verdict} |"
        )
    overall = "PASS" if summary["overall_passed"] else "FAIL"
    lines.append("")
    lines.append(
        f"**Overall Z4/S1 gate:** {overall} | "
        f"**Recommendation:** `{summary['recommendation']}` | "
        f"**default_flip_allowed:** `{summary['default_flip_allowed']}`"
    )
    return "\n".join(lines)


def build_plan(
    clip_50p: str = "<clip_50p.mkv>",
    clip_60p: str = "<clip_60p.mkv>",
    out_dir: str = "logs/motion_ahead_z4",
    vp_exe: str = "VideoProcessor.exe",
) -> Dict[str, Any]:
    runs: List[Dict[str, Any]] = []
    for rate, clip in (("50p", clip_50p), ("60p", clip_60p)):
        hz, budget_ms = RATES[rate]
        for arm, key_val in (("baseline", "off"), ("candidate", "on")):
            log_path = f"{out_dir}/{rate}_{arm}.log"
            cmd = (
                f"{vp_exe} --input \"{clip}\" "
                f"--set motion_estimate_ahead_spare={key_val} "
                f"--log \"{log_path}\""
            )
            runs.append(
                {
                    "rate": rate,
                    "hz": hz,
                    "budget_ms": round(budget_ms, 3),
                    "arm": arm,
                    "motion_estimate_ahead_spare": key_val,
                    "log_path": log_path,
                    "command": cmd,
                }
            )

    parse_cmd = (
        f"python3 tools/motion_ahead/run_50_60p_cost.py --mode parse "
        f"--baseline-50p \"{out_dir}/50p_baseline.log\" "
        f"--candidate-50p \"{out_dir}/50p_candidate.log\" "
        f"--baseline-60p \"{out_dir}/60p_baseline.log\" "
        f"--candidate-60p \"{out_dir}/60p_candidate.log\" "
        f"--json-out \"{out_dir}/z4_summary.json\""
    )
    return {
        "mode": "plan",
        "default_flip_allowed": DEFAULT_FLIP_ALLOWED,
        "budgets_ms": {
            "50p": round(BUDGET_50P_MS, 3),
            "60p": round(BUDGET_60P_MS, 3),
        },
        "runs": runs,
        "parse_command": parse_cmd,
    }


def format_plan_text(plan: Mapping[str, Any]) -> str:
    lines = [
        "# Z4 / S1 50p & 60p Motion-Estimate-Ahead-Spare A/B Run Plan",
        f"- 50p frame budget: {plan['budgets_ms']['50p']:.3f} ms (1000 / 50)",
        f"- 60p frame budget: {plan['budgets_ms']['60p']:.3f} ms (1000 / 60)",
        f"- default_flip_allowed: {plan['default_flip_allowed']} (NO default flip; recommendation only)",
        "",
        "## A/B Capture Commands",
    ]
    for idx, run in enumerate(plan["runs"], start=1):
        lines.append(
            f"{idx}. [{run['rate']} / {run['arm']} | budget {run['budget_ms']:.3f} ms | "
            f"motion_estimate_ahead_spare={run['motion_estimate_ahead_spare']}]"
        )
        lines.append(f"   {run['command']}")
    lines.append("")
    lines.append("## Parse & Gate Evaluation Command")
    lines.append(f"   {plan['parse_command']}")
    return "\n".join(lines)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Z4/S1 50p & 60p motion_estimate_ahead_spare A/B cost line runner & ledger parser (no default flip)."
    )
    p.add_argument("--mode", choices=("plan", "parse"), required=True, help="Execution mode: plan or parse.")
    p.add_argument("--input-format", choices=("auto", "log", "csv", "json"), default="auto")
    p.add_argument("--baseline-50p", type=Path, default=None, help="50p baseline (motion_estimate_ahead_spare=off) log/CSV/JSON.")
    p.add_argument("--candidate-50p", type=Path, default=None, help="50p candidate (motion_estimate_ahead_spare=on) log/CSV/JSON.")
    p.add_argument("--baseline-60p", type=Path, default=None, help="60p baseline (motion_estimate_ahead_spare=off) log/CSV/JSON.")
    p.add_argument("--candidate-60p", type=Path, default=None, help="60p candidate (motion_estimate_ahead_spare=on) log/CSV/JSON.")
    p.add_argument("--clip-50p", default="<clip_50p.mkv>", help="50p clip path for --mode plan.")
    p.add_argument("--clip-60p", default="<clip_60p.mkv>", help="60p clip path for --mode plan.")
    p.add_argument("--out-dir", default="logs/motion_ahead_z4", help="Output directory for --mode plan.")
    p.add_argument("--vp-exe", default="VideoProcessor.exe", help="Executable name for --mode plan.")
    p.add_argument("--format", choices=("markdown", "json", "both"), default="both", help="Stdout format for --mode parse.")
    p.add_argument("--json-out", type=Path, default=None, help="Optional path to write deterministic JSON summary.")
    p.add_argument("--strict", action="store_true", help="Exit 2 in --mode parse when overall Z4/S1 gate fails.")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.mode == "plan":
        plan = build_plan(
            clip_50p=args.clip_50p,
            clip_60p=args.clip_60p,
            out_dir=args.out_dir,
            vp_exe=args.vp_exe,
        )
        if args.json_out is not None:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if args.format == "json":
            print(json.dumps(plan, indent=2, sort_keys=True))
        else:
            print(format_plan_text(plan))
        return 0

    rate_evals: Dict[str, RateEvaluation] = {}
    pairs = (
        ("50p", args.baseline_50p, args.candidate_50p),
        ("60p", args.baseline_60p, args.candidate_60p),
    )
    for rate, base_path, cand_path in pairs:
        if base_path is None and cand_path is None:
            continue
        if base_path is None or cand_path is None:
            parser.error(f"--baseline-{rate} and --candidate-{rate} must be provided together.")
        base_m = parse_run_file(base_path, input_format=args.input_format)
        cand_m = parse_run_file(cand_path, input_format=args.input_format)
        rate_evals[rate] = evaluate_rate(rate, base_m, cand_m)

    if not rate_evals:
        parser.error("--mode parse requires at least one (--baseline-50p/--candidate-50p or --baseline-60p/--candidate-60p) pair.")

    summary = evaluate_ab(rate_evals)
    json_text = json.dumps(summary, indent=2, sort_keys=True)

    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json_text + "\n", encoding="utf-8")

    if args.format in ("markdown", "both"):
        print(format_markdown_ledger(summary))
    if args.format == "both":
        print("")
    if args.format in ("json", "both"):
        print(json_text)

    if args.strict and not summary["overall_passed"]:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

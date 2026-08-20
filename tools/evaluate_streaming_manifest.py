#!/usr/bin/env python3
"""Evaluate the streaming executable against a JSONL speech manifest."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
import unicodedata
import wave
from collections import defaultdict
from pathlib import Path
from typing import Any

try:
    from opencc import OpenCC
except ImportError:
    OpenCC = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--vad", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--replay-speed", type=float, default=20.0)
    parser.add_argument("--endpoint-silence-ms", type=int, default=700)
    parser.add_argument("--maximum-utterance-ms", type=int, default=30_000)
    parser.add_argument("--tail-silence-ms", type=int, default=1_800)
    parser.add_argument("--vad-speech-threshold", type=float)
    parser.add_argument("--vad-min-db", type=float)
    parser.add_argument("--vad-min-snr-db", type=float)
    parser.add_argument("--vad-min-speech-ms", type=int)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--per-locale-limit", type=int)
    parser.add_argument("--per-locale-offset", type=int, default=0)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--retry-failed", action="store_true")
    parser.add_argument("--timeout-seconds", type=int, default=120)
    return parser.parse_args()


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    if not path.exists():
        return entries
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                value = json.loads(stripped)
            except json.JSONDecodeError as error:
                raise ValueError(f"Malformed JSON at {path}:{line_number}: {error}") from error
            if not isinstance(value, dict):
                raise ValueError(f"Expected an object at {path}:{line_number}")
            entries.append(value)
    return entries


def audio_duration_ms(audio_path: Path) -> int:
    with wave.open(str(audio_path), "rb") as input_file:
        return round(input_file.getnframes() * 1_000 / input_file.getframerate())


def normalize_text(value: str, converter: Any | None) -> str:
    if converter is not None:
        value = converter.convert(value)
    return "".join(
        character
        for character in value
        if not character.isspace() and not unicodedata.category(character).startswith("P")
    )


def levenshtein_distance(left: str, right: str) -> int:
    if len(left) < len(right):
        left, right = right, left
    previous = list(range(len(right) + 1))
    for left_index, left_character in enumerate(left, start=1):
        current = [left_index]
        for right_index, right_character in enumerate(right, start=1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[right_index] + 1,
                    previous[right_index - 1]
                    + (left_character != right_character),
                )
            )
        previous = current
    return previous[-1]


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def command_for(arguments: argparse.Namespace, audio_path: Path) -> list[str]:
    command = [
        str(arguments.executable),
        "--model",
        str(arguments.model),
        "--vad",
        str(arguments.vad),
        "--audio",
        str(audio_path),
        "--threads",
        str(arguments.threads),
        "--replay-speed",
        str(arguments.replay_speed),
        "--endpoint-silence-ms",
        str(arguments.endpoint_silence_ms),
        "--maximum-utterance-ms",
        str(arguments.maximum_utterance_ms),
        "--replay-tail-silence-ms",
        str(arguments.tail_silence_ms),
    ]
    optional_arguments = (
        ("--vad-speech-threshold", arguments.vad_speech_threshold),
        ("--vad-min-db", arguments.vad_min_db),
        ("--vad-min-snr-db", arguments.vad_min_snr_db),
        ("--vad-min-speech-ms", arguments.vad_min_speech_ms),
    )
    for option, value in optional_arguments:
        if value is not None:
            command.extend((option, str(value)))
    return command


def run_sample(
    arguments: argparse.Namespace,
    sample: dict[str, Any],
    converter: Any | None,
) -> dict[str, Any]:
    audio_path = Path(str(sample["audio"]))
    reference = str(sample["reference"])
    started_at = time.perf_counter()
    try:
        completed = subprocess.run(
            command_for(arguments, audio_path),
            capture_output=True,
            timeout=arguments.timeout_seconds,
            check=False,
        )
        elapsed_ms = round((time.perf_counter() - started_at) * 1_000)
    except subprocess.TimeoutExpired as error:
        elapsed_ms = round((time.perf_counter() - started_at) * 1_000)
        return {
            **sample,
            "status": "timeout",
            "elapsed_ms": elapsed_ms,
            "error": f"Timed out after {arguments.timeout_seconds} seconds: {error}",
        }
    stdout = completed.stdout.decode("utf-8", errors="replace")
    stderr = completed.stderr.decode("utf-8", errors="replace")
    events: list[dict[str, Any]] = []
    invalid_stdout: list[str] = []
    for line in stdout.splitlines():
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            invalid_stdout.append(line)
            continue
        if isinstance(event, dict):
            events.append(event)
    final_events = [event for event in events if event.get("event") == "final"]
    hypothesis = "".join(str(event.get("text", "")) for event in final_events)
    normalized_reference = normalize_text(reference, converter)
    normalized_hypothesis = normalize_text(hypothesis, converter)
    errors = levenshtein_distance(normalized_reference, normalized_hypothesis)
    reference_characters = len(normalized_reference)
    return {
        **sample,
        "status": "ok" if completed.returncode == 0 else "command_failed",
        "return_code": completed.returncode,
        "elapsed_ms": elapsed_ms,
        "audio_ms": audio_duration_ms(audio_path),
        "reference_normalized": normalized_reference,
        "hypothesis": hypothesis,
        "hypothesis_normalized": normalized_hypothesis,
        "reference_characters": reference_characters,
        "errors": errors,
        "cer": errors / reference_characters if reference_characters else 0.0,
        "final_segments": len(final_events),
        "forced_split": any(bool(event.get("forced_split")) for event in final_events),
        "inference_ms": sum(int(event.get("inference_ms", 0)) for event in final_events),
        "vad_ms": sum(int(event.get("vad_ms", 0)) for event in final_events),
        "event_errors": [event.get("error", "") for event in events if event.get("event") == "error"],
        "stderr": stderr.strip(),
        "invalid_stdout": invalid_stdout,
    }


def aggregate(entries: list[dict[str, Any]]) -> dict[str, Any]:
    successful = [entry for entry in entries if entry.get("status") == "ok"]
    total_reference_characters = sum(entry.get("reference_characters", 0) for entry in successful)
    total_errors = sum(entry.get("errors", 0) for entry in successful)
    total_audio_ms = sum(entry.get("audio_ms", 0) for entry in successful)
    total_inference_ms = sum(entry.get("inference_ms", 0) for entry in successful)
    elapsed_values = [entry.get("elapsed_ms", 0) for entry in successful]
    inference_values = [entry.get("inference_ms", 0) for entry in successful]
    final_values = [entry.get("final_segments", 0) for entry in successful]
    with_final = [entry for entry in successful if entry.get("final_segments", 0) > 0]
    return {
        "samples": len(entries),
        "successful_samples": len(successful),
        "failed_samples": len(entries) - len(successful),
        "samples_with_final": len(with_final),
        "missing_final_rate": (len(successful) - len(with_final)) / len(successful)
        if successful
        else 0.0,
        "forced_split_count": sum(bool(entry.get("forced_split")) for entry in successful),
        "forced_split_rate": sum(bool(entry.get("forced_split")) for entry in successful)
        / len(successful)
        if successful
        else 0.0,
        "reference_characters": total_reference_characters,
        "errors": total_errors,
        "cer": total_errors / total_reference_characters if total_reference_characters else 0.0,
        "total_audio_ms": total_audio_ms,
        "total_inference_ms": total_inference_ms,
        "inference_realtime_factor": total_inference_ms / total_audio_ms
        if total_audio_ms
        else 0.0,
        "mean_elapsed_ms": round(statistics.fmean(elapsed_values), 1) if elapsed_values else 0.0,
        "p95_elapsed_ms": percentile(elapsed_values, 0.95),
        "mean_inference_ms": round(statistics.fmean(inference_values), 1)
        if inference_values
        else 0.0,
        "p95_inference_ms": percentile(inference_values, 0.95),
        "mean_final_segments": round(statistics.fmean(final_values), 3) if final_values else 0.0,
    }


def main() -> int:
    arguments = parse_arguments()
    for input_path in (arguments.executable, arguments.model, arguments.vad, arguments.manifest):
        if not input_path.exists():
            raise SystemExit(f"Missing required path: {input_path}")
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    result_path = arguments.output_dir / "results.jsonl"
    summary_path = arguments.output_dir / "summary.json"
    manifest = read_jsonl(arguments.manifest)
    if arguments.per_locale_limit is not None:
        selected: list[dict[str, Any]] = []
        locale_counts: dict[str, int] = defaultdict(int)
        for sample in manifest:
            locale = str(sample.get("locale", "unknown"))
            locale_counts[locale] += 1
            position = locale_counts[locale] - 1
            if position < arguments.per_locale_offset:
                continue
            if position >= arguments.per_locale_offset + arguments.per_locale_limit:
                continue
            selected.append(sample)
        manifest = selected
    if arguments.limit is not None:
        manifest = manifest[: arguments.limit]
    existing = [] if arguments.overwrite else read_jsonl(result_path)
    existing_by_audio = {str(entry.get("audio")): entry for entry in existing}
    retained = [
        entry
        for entry in existing
        if str(entry.get("audio")) in {str(sample.get("audio")) for sample in manifest}
        and (entry.get("status") == "ok" or not arguments.retry_failed)
    ]
    completed_audio = {str(entry.get("audio")) for entry in retained}
    pending = [sample for sample in manifest if str(sample.get("audio")) not in completed_audio]
    converter = OpenCC("t2s") if OpenCC is not None else None
    print(
        f"Evaluating {len(pending)} pending samples; retaining {len(retained)} existing results "
        f"(traditional-to-simplified normalization: {'on' if converter else 'off'}).",
        flush=True,
    )
    all_results = {str(entry["audio"]): entry for entry in retained}
    with result_path.open("w", encoding="utf-8") as output_file:
        for entry in retained:
            output_file.write(json.dumps(entry, ensure_ascii=False) + "\n")
        output_file.flush()
        for index, sample in enumerate(pending, start=1):
            result = run_sample(arguments, sample, converter)
            all_results[str(sample["audio"])] = result
            output_file.write(json.dumps(result, ensure_ascii=False) + "\n")
            output_file.flush()
            if index % 10 == 0 or index == len(pending):
                print(
                    f"Completed {index}/{len(pending)} pending samples "
                    f"({sample.get('locale', 'unknown')}, status={result['status']}, "
                    f"CER={result.get('cer', 0.0):.4f}).",
                    flush=True,
                )
    ordered_results = [all_results[str(sample["audio"])] for sample in manifest]
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for entry in ordered_results:
        grouped[str(entry.get("locale", "unknown"))].append(entry)
    summary = {
        "configuration": {
            "executable": str(arguments.executable),
            "model": str(arguments.model),
            "vad": str(arguments.vad),
            "threads": arguments.threads,
            "replay_speed": arguments.replay_speed,
            "endpoint_silence_ms": arguments.endpoint_silence_ms,
            "maximum_utterance_ms": arguments.maximum_utterance_ms,
            "tail_silence_ms": arguments.tail_silence_ms,
            "vad_speech_threshold": arguments.vad_speech_threshold,
            "vad_min_db": arguments.vad_min_db,
            "vad_min_snr_db": arguments.vad_min_snr_db,
            "vad_min_speech_ms": arguments.vad_min_speech_ms,
            "per_locale_limit": arguments.per_locale_limit,
            "per_locale_offset": arguments.per_locale_offset,
            "traditional_to_simplified_normalization": converter is not None,
        },
        "overall": aggregate(ordered_results),
        "by_locale": {locale: aggregate(entries) for locale, entries in sorted(grouped.items())},
        "worst_samples": sorted(
            ordered_results,
            key=lambda entry: (entry.get("status") != "ok", entry.get("cer", 0.0)),
            reverse=True,
        )[:20],
    }
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary["overall"], ensure_ascii=False, indent=2))
    print(f"Wrote {result_path} and {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

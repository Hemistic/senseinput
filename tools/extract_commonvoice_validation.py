#!/usr/bin/env python3
"""Extract audio and references from ModelScope Common Voice Parquet shards."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import pyarrow.parquet as parquet


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def extract_shard(shard_path: Path, output_dir: Path, manifest) -> int:
    shard_name = shard_path.name.removesuffix("-validation.parquet")
    row_count = 0
    reader = parquet.ParquetFile(shard_path)
    for batch in reader.iter_batches(columns=["audio", "sentence", "locale", "path"]):
        for row in batch.to_pylist():
            audio = row["audio"] or {}
            audio_bytes = audio.get("bytes")
            sentence = (row.get("sentence") or "").strip()
            locale = (row.get("locale") or shard_name).strip()
            if not audio_bytes or not sentence:
                continue

            audio_path = output_dir / locale / f"{row_count:04d}.wav"
            audio_path.parent.mkdir(parents=True, exist_ok=True)
            audio_path.write_bytes(audio_bytes)
            manifest.write(
                json.dumps(
                    {
                        "audio": str(audio_path),
                        "reference": sentence,
                        "locale": locale,
                        "source_path": row.get("path") or "",
                    },
                    ensure_ascii=False,
                )
                + "\n"
            )
            row_count += 1
    return row_count


def main() -> int:
    arguments = parse_arguments()
    shards = sorted(arguments.input_dir.glob("*-validation.parquet"))
    if not shards:
        raise SystemExit(f"No validation shards found in {arguments.input_dir}")

    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = arguments.output_dir / "manifest.jsonl"
    total = 0
    with manifest_path.open("w", encoding="utf-8") as manifest:
        for shard in shards:
            count = extract_shard(shard, arguments.output_dir, manifest)
            total += count
            print(f"{shard.name}: {count} samples")
    print(f"Wrote {total} samples to {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Validate the AlgaGuard 16 MB dual-slot OTA partition table."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Partition:
    name: str
    kind: str
    subtype: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


def load_partitions(path: Path) -> list[Partition]:
    rows: list[Partition] = []
    with path.open(encoding="utf-8", newline="") as source:
        filtered = (line for line in source if not line.lstrip().startswith("#"))
        for row in csv.reader(filtered):
            if not row or all(not value.strip() for value in row):
                continue
            if len(row) < 5:
                raise ValueError(f"partition row has fewer than five fields: {row}")
            rows.append(
                Partition(
                    name=row[0].strip(),
                    kind=row[1].strip(),
                    subtype=row[2].strip(),
                    offset=int(row[3].strip(), 0),
                    size=int(row[4].strip(), 0),
                )
            )
    return rows


def validate_partitions(
    partitions: list[Partition],
    flash_size: int,
    firmware_size: int,
    safety_margin: int,
) -> None:
    if flash_size != 16 * 1024 * 1024:
        raise ValueError("configured flash size must be exactly 16 MB")
    if not any(item.kind == "data" and item.subtype == "ota" for item in partitions):
        raise ValueError("OTA data partition is required")
    app_slots = [
        item
        for item in partitions
        if item.kind == "app" and item.subtype in {"ota_0", "ota_1"}
    ]
    if len(app_slots) < 2:
        raise ValueError("two OTA application slots are required")
    if len({item.offset for item in partitions}) != len(partitions):
        raise ValueError("partition offsets must be unique")
    ordered = sorted(partitions, key=lambda item: item.offset)
    for previous, current in zip(ordered, ordered[1:]):
        if previous.end > current.offset:
            raise ValueError(f"partition {previous.name} overlaps {current.name}")
    if max(item.end for item in partitions) > flash_size:
        raise ValueError("partition table exceeds configured flash size")
    required_slot_size = firmware_size + safety_margin
    for slot in app_slots:
        if slot.size < required_slot_size:
            raise ValueError(
                f"{slot.name} is too small: {slot.size} < {required_slot_size}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("partition_table", type=Path)
    parser.add_argument("--flash-size", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--firmware-size", type=lambda value: int(value, 0), default=0)
    parser.add_argument(
        "--safety-margin", type=lambda value: int(value, 0), default=0x100000
    )
    arguments = parser.parse_args()
    firmware_size = (
        arguments.firmware.stat().st_size
        if arguments.firmware is not None
        else arguments.firmware_size
    )
    partitions = load_partitions(arguments.partition_table)
    validate_partitions(
        partitions, arguments.flash_size, firmware_size, arguments.safety_margin
    )
    app_slots = [item for item in partitions if item.kind == "app"]
    print(
        "partition validation passed:",
        f"flash={arguments.flash_size}",
        f"firmware={firmware_size}",
        f"ota_slots={','.join(str(item.size) for item in app_slots)}",
        f"margin={arguments.safety_margin}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

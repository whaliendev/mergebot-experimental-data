#!/bin/env python3
import json
import os
import sys
from pathlib import Path

def split_filename(filepath):
    base_name = os.path.basename(filepath)
    name, ext = os.path.splitext(base_name)
    return os.path.join(os.path.dirname(filepath), name), ext

def count_conflicts(results_dir_path: Path):
    conflict_files = 0
    conflicts = 0

    statistics_json_name = "statistics.json"
    for root, dirs, files in os.walk(results_dir_path):
        if statistics_json_name in files:
            metadata_json_path = os.path.join(root, statistics_json_name)
            with open(metadata_json_path, "r") as f:
                metadata = json.load(f)

                conflict_sources = metadata["conflict_sources"]
                if not conflict_sources:
                    print("warning: no conflict sources found in statistics.json in directory: ", root)
                    continue

                conflict_files += len(conflict_sources)

                for conflict_source in conflict_sources:
                    conflict_file_str = conflict_source["file_path"]
                    if not conflict_file_str:
                        print("warning: no file path found in conflict source in statistics.json in directory: ", root)
                        continue

                    stem, suffix = split_filename(conflict_file_str)
                    stem_escaped = stem.replace(os.sep, "@")
                    raw_conflict_file = f"{stem_escaped}.raw{suffix}"

                    conflict_file_path = os.path.join(root, raw_conflict_file)

                    if not os.path.exists(conflict_file_path):
                        print("warning: conflict file does not exist: ", conflict_file_path)
                        continue

                    with open(conflict_file_path, "r") as f:
                        for line in f:
                            if line.startswith("<<<<<<<"):
                                conflicts += 1

    return conflict_files, conflicts

if __name__ == "__main__":
    if len(sys.argv) <= 1:
        print(f"Usage: python {sys.argv[0]} <results_dir>")
        exit(1)

    results_dir = sys.argv[1]
    results_dir_path = Path(results_dir)
    if not results_dir_path.exists():
        print(f"error: results directory does not exist: {results_dir}")
        exit(1)

    conflict_files, conflicts = count_conflicts(results_dir_path)

    print(f"conflict files: {conflict_files}, conflicts: {conflicts}")
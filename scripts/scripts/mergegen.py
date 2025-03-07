import glob
import json
import re
import requests
import os
from os.path import join
from typing import List
from octok.utils.gitservice import process_conflict_blocks

base_url = "127.0.0.1:5000/resolve_conflict"

data = {
    "raw_a": "",
    "raw_b": "",
    "raw_base": ""
}

result_dir = "/home/whalien/codebase/python/mergebot-eva/output"


def remove_whitespace_and_compare(str1, str2):
    """Remove all whitespace characters (including spaces, tabs, and newlines) and compare."""
    str1_no_whitespace = re.sub(r'\s+', '', str1)
    str2_no_whitespace = re.sub(r'\s+', '', str2)
    return str1_no_whitespace == str2_no_whitespace


def get_leading_context(conflicting_lines: List[str], start_line: int, context_size: int = 10) -> str:
    """Get the leading context of the conflict block."""
    start_line = max(0, start_line - context_size + 1)
    return "\n".join(conflicting_lines[start_line: start_line + context_size])


def get_trailing_context(conflicting_lines: List[str], end_line: int, context_size: int = 10) -> str:
    """Get the trailing context of the conflict block."""
    return "\n".join(conflicting_lines[end_line: end_line + context_size])


for repo in os.listdir(result_dir):
    conflict_blocks = 0
    resolved_counter = 0
    repo_dir = join(result_dir, repo)
    mss = os.listdir(repo_dir)
    for ms in mss:
        ms_dir = join(repo_dir, ms)
        raw_conflict_pattern = "*raw.{cc,h,inl,hpp,C,cpp,cxx,c}"
        raw_conflict_files = glob.glob(join(ms_dir, raw_conflict_pattern))

        for raw_conflict in raw_conflict_files:
            # Get the filename component of the path and replace "raw" with "merged"
            filename = os.path.basename(raw_conflict)
            merged_filename = filename.replace("raw", "merged")

            # Get the full path with the new filename
            merged_filepath = os.path.join(os.path.dirname(raw_conflict), merged_filename)

            if not os.path.exists(merged_filepath):
                print(f"!!!! warning: corresponding merged file not found for {raw_conflict} in {ms_dir}")
                continue

            with open(merged_filepath, "r") as f:
                merged_lines = f.readlines()

            with open(raw_conflict, "r") as f:
                conflict_lines = f.readlines()

            conflict_blocks = process_conflict_blocks(conflict_lines, merged_lines)

            conflicting_chunks = [{
                "a_contents": conflict_block.ours,
                "b_contents": conflict_block.theirs,
                "base_contents": conflict_block.base,
                "res_region": conflict_block.merged,
                "lookback": get_leading_context(merged_lines, conflict_block.resolved_start_line, 3),
                "lookahead": get_trailing_context(merged_lines, conflict_block.resolved_end_line, 2),
                "label": conflict_block.labels
            } for conflict_block in conflict_blocks]
            conflict_blocks += len(conflicting_chunks)

            for conflict_chunk in conflicting_chunks:
                data.clear()
                data["raw_a"] = conflict_chunk["lookback"] + conflict_chunk["a_contents"] + conflict_chunk["lookahead"]
                data["raw_b"] = conflict_chunk["lookback"] + conflict_chunk["b_contents"] + conflict_chunk["lookahead"]
                data["raw_base"] = conflict_chunk["lookback"] + conflict_chunk["base_contents"] + conflict_chunk[
                    "lookahead"]

                response = requests.post(base_url, json=data)

                if response.status_code != 200:
                    print(f"Failed to resolve conflict for {raw_conflict} in {ms_dir}")
                    continue
                else:
                    resolved_content = response.json()["resolved"]
                    conflict_chunk["merge_gen_region"] = resolved_content
                    conflict_chunk["resolved"] = remove_whitespace_and_compare(resolved_content,
                                                                               conflict_chunk["res_region"])
                    if conflict_chunk["resolved"]:
                        resolved_counter += 1

            # After processing conflicts, dump the conflicting_chunks to a JSON file
            conflict_json_filename = os.path.basename(raw_conflict) + ".resolution.json"
            conflict_json_filepath = os.path.join(ms_dir, conflict_json_filename)

            # Write the conflict chunks to the JSON file
            with open(conflict_json_filepath, 'w') as json_file:
                json.dump(conflicting_chunks, json_file, indent=4)

            print(f"Conflicting chunks for {raw_conflict} saved to {conflict_json_filepath}")
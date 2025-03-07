#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from os.path import join
from typing import Any, Dict, List

import octok.log
from octok.evaluate import analyze_diff_lines, get_diff_lines
from octok.model import EvaluateDecoder, MergeScenarioStatistics
from octok.utils.fileop import remove_cpp_comments

log = octok.log.get_logger(__name__)

def generate_mergegen_results(
        raw_conflict_file_dest: str,
        mergegen_results_json: str,
        mergegen_results_dest: str,
):
    raw_conflict_lines = ""
    with open(raw_conflict_file_dest, "r") as f:
        raw_conflict_lines = f.read()

    mergegen_results = []
    with open(mergegen_results_json, "r") as f:
        mergegen_results = json.load(f)

    mergegen_regions = [f"\n{mergegen_resolution["merge_gen_region"]}\n" for mergegen_resolution in mergegen_results]

    conflict_pattern = re.compile(
        r'<<<<<<< .*?\n'      # 冲突开始标志
        r'(.*?)'             # HEAD版本的内容
        r'=======\n'            # 分隔符
        r'(.*?)'             # 另一个分支的内容
        r'>>>>>>> .*?\n',        # 冲突结束标志
        re.DOTALL
    )
    conflicts = list(conflict_pattern.finditer(raw_conflict_lines))
    assert len(conflicts) <= len(mergegen_results), f"Conflict count mismatch: {len(conflicts)} <= {len(mergegen_results)}"

    def replacement_function(match, replacements=iter(mergegen_regions)):
        try:
            return next(replacements)
        except StopIteration:
            return match.group(0)

    replaced_content = conflict_pattern.sub(replacement_function, raw_conflict_lines)

    with open(mergegen_results_dest, "w") as f:
        f.write(replaced_content)


def count_lines(raw_conflict_no_comments):
    with open(raw_conflict_no_comments, "r") as f:
        return len(f.readlines())


def process_conflict_source(
        conflict_source: Any,
        merge_scenario_dir: str,
        project: str,
        merge_scenario: str,
) -> Dict[str, Any]:
    """
    Process a single conflict source and collect statistics.

    Args:
        conflict_source: The conflict source object.
        merge_scenario_dir: Path to the merge scenario directory.
        project: Project name.
        merge_scenario: Merge scenario name.

    Returns:
        A dictionary containing statistics for the conflict source.
    """
    log.info(
        f"Processing {project}, merge scenario: {merge_scenario}, "
        f"conflict source: {conflict_source.file_path}..."
    )

    # Prepare file paths
    basename, file_ext = os.path.splitext(conflict_source.file_path)
    basename_escaped = basename.replace(os.path.sep, '@')
    raw_conflict_file_dest = join(
        merge_scenario_dir, f"{basename_escaped}.raw{file_ext}"
    ) # 待替换文件

    if not os.path.exists(raw_conflict_file_dest):
        log.error(f"Conflict file {raw_conflict_file_dest} does not exist")
        return {}

    mergegen_results_json = join(
        merge_scenario_dir, f"{basename_escaped}.mergegen{file_ext}.json"
    )
    if not os.path.exists(mergegen_results_json):
        log.error(f"Mergegen results file {mergegen_results_json} does not exist")
        return {}

    mergegen_results_dest = join(
        merge_scenario_dir, f"{basename_escaped}.mergegen{file_ext}"
    )

    generate_mergegen_results(raw_conflict_file_dest, mergegen_results_json, mergegen_results_dest)

    mergegen_conflict_no_comments = join(
        merge_scenario_dir, f"{basename_escaped}.no_comments_mergegen{file_ext}"
    ) # 去除 comment 的文件
    merged_no_comments = join(
        merge_scenario_dir, f"{basename_escaped}.no_comments_merged{file_ext}"
    ) # 事实

    remove_cpp_comments(mergegen_results_dest, mergegen_conflict_no_comments)

    if not os.path.isfile(merged_no_comments):
        log.error(f"Merged file {merged_no_comments} does not exist")
        return {}

    # Analyze differences
    diff_file = f"{basename_escaped}.mergegen"
    diff_lines = get_diff_lines(
        merged_no_comments,
        mergegen_conflict_no_comments,
        merge_scenario_dir,
        diff_file,
    )
    diff_line_count = analyze_diff_lines(diff_lines, merge_scenario_dir, f"{diff_file}.processed")

    return {
        "file_path": conflict_source.file_path,
        "merged_line_count": conflict_source.merged_line_count,
        "mergegen_conflict_line_count": count_lines(mergegen_conflict_no_comments),
        "diff_line_count": diff_line_count,
    }


def add_mergegen_results_to_project(outdir: str, project: str) -> None:
    """
    Process all merge scenarios for a given project.

    Args:
        outdir: Output directory containing projects.
        project: Name of the project to process.
    """
    project_out_dir = join(outdir, project)
    try:
        _, merge_scenarios, _ = next(os.walk(project_out_dir))
    except StopIteration:
        log.error(f"No merge scenarios found in {project_out_dir}")
        return

    for merge_scenario in merge_scenarios:
        merge_scenario_parts = merge_scenario.split("-")
        if len(merge_scenario_parts) != 3:
            log.error(f"Invalid merge scenario {merge_scenario}")
            continue

        merge_scenario_dir = join(project_out_dir, merge_scenario)
        statistics_file = join(merge_scenario_dir, "statistics.json")

        # Load statistics
        try:
            with open(statistics_file, "r") as f:
                mergebot_stats: MergeScenarioStatistics = json.load(
                    f, cls=EvaluateDecoder
                )
        except Exception as e:
            log.error(f"Failed to read {statistics_file}: {e}")
            continue

        conflict_source_list: List[Dict[str, Any]] = []
        for conflict_source in mergebot_stats.conflict_sources:
            stats = process_conflict_source(
                conflict_source, # relative path to the conflict file
                merge_scenario_dir, # physical merge scenario dir
                project, # project name
                merge_scenario, # hash name
            )
            if stats:
                conflict_source_list.append(stats)

        # Calculate overall statistics
        total_diff_line_count = sum(
            cs.get("diff_line_count", 0) for cs in conflict_source_list
        )
        total_merged_line_count = sum(
            cs.get("merged_line_count", 0) for cs in conflict_source_list
        )
        total_mergegen_conflict_line_count = sum(
            cs.get("mergegen_conflict_line_count", 0) for cs in conflict_source_list
        )

        precision = (
            (total_mergegen_conflict_line_count - total_diff_line_count)
            / total_mergegen_conflict_line_count
            if total_mergegen_conflict_line_count > 0
            else None
        )
        recall = (
            (total_mergegen_conflict_line_count - total_diff_line_count)
            / total_merged_line_count
            if total_merged_line_count > 0
            else None
        )

        mergegen_conflict_statistics = {
            "conflict_sources": conflict_source_list,
            "total_diff_line_count": total_diff_line_count,
            "total_merged_line_count": total_merged_line_count,
            "total_mergegen_conflict_line_count": total_mergegen_conflict_line_count,
            "precision": precision,
            "recall": recall,
        }

        # Write statistics to file
        mergegen_conflict_stats_file = join(
            merge_scenario_dir, "mergegen_conflict_statistics.json"
        )
        log.info(f"mergebot diff lines: {mergebot_stats.total_diff_line_count}, mergegen diff lines: {mergegen_conflict_statistics["total_diff_line_count"]}")
        try:
            with open(mergegen_conflict_stats_file, "w") as f:
                json.dump(mergegen_conflict_statistics, f, indent=4)
        except Exception as e:
            log.error(f"Failed to write {mergegen_conflict_stats_file}: {e}")


def main():
    parser = argparse.ArgumentParser(
        description="Process raw conflicts for projects."
    )
    parser.add_argument("output_dir", help="Output directory containing projects")
    parser.add_argument("--project_name", help="Specific project to process", default=None)
    args = parser.parse_args()

    output_dir = args.output_dir
    project_name = args.project_name

    if not os.path.isdir(output_dir):
        log.error(f"{output_dir} is not a directory")
        sys.exit(1)

    if project_name:
        project_dir = join(output_dir, project_name)
        if os.path.isdir(project_dir):
            add_mergegen_results_to_project(output_dir, project_name)
        else:
            log.error(f"Project '{project_name}' does not exist in {output_dir}")
            sys.exit(1)
    else:
        for project in os.listdir(output_dir):
            project_dir = join(output_dir, project)
            if os.path.isdir(project_dir):
                add_mergegen_results_to_project(output_dir, project)


if __name__ == "__main__":
    main()

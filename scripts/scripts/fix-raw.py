#!/usr/bin/env python3
import argparse
import json
import os
import sys
from os.path import join
from typing import Any, Dict, List

import octok.log
from octok.evaluate import analyze_diff_lines, get_diff_lines
from octok.model import EvaluateDecoder, MergeScenarioStatistics
from octok.utils.fileop import remove_cpp_comments
from octok.utils.git import (
    clear_git_stash,
    git_checkout,
    git_merge,
    git_merge_abort,
    git_stash_changes,
)
from octok.utils.gitservice import get_repo

log = octok.log.get_logger(__name__)

SAMPLE_BASE = '/home/whalien/sample-repos/'


def clean_git_repo(repo_path: str) -> None:
    """Abort any ongoing merge and clean up the git repository."""
    git_merge_abort(repo_path)
    git_stash_changes(repo_path)
    clear_git_stash(repo_path)


def copy_content_to(src: str, dest: str) -> None:
    """Copy content from the source file to the destination file."""
    with open(src, "r") as src_f, open(dest, "w") as dest_f:
        dest_f.write(src_f.read())


def count_lines(raw_conflict_no_comments):
    with open(raw_conflict_no_comments, "r") as f:
        return len(f.readlines())


def process_conflict_source(
        conflict_source: Any,
        repo_path: str,
        merge_scenario_dir: str,
        project: str,
        merge_scenario: str,
) -> Dict[str, Any]:
    """
    Process a single conflict source and collect statistics.

    Args:
        conflict_source: The conflict source object.
        repo_path: Path to the git repository.
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

    conflict_file_path = join(repo_path, conflict_source.file_path)
    if not os.path.isfile(conflict_file_path):
        log.error(f"Conflict file {conflict_file_path} does not exist")
        return {}

    # Prepare file paths
    basename, file_ext = os.path.splitext(conflict_source.file_path)
    basename_escaped = basename.replace(os.path.sep, '@')
    raw_conflict_file_dest = join(
        merge_scenario_dir, f"{basename_escaped}.raw{file_ext}"
    )
    raw_conflict_no_comments = join(
        merge_scenario_dir, f"{basename_escaped}.no_comments_conflict{file_ext}"
    )
    merged_no_comments = join(
        merge_scenario_dir, f"{basename_escaped}.no_comments_merged{file_ext}"
    )

    # Copy and process files
    copy_content_to(conflict_file_path, raw_conflict_file_dest)
    remove_cpp_comments(raw_conflict_file_dest, raw_conflict_no_comments)

    if not os.path.isfile(merged_no_comments):
        log.error(f"Merged file {merged_no_comments} does not exist")
        return {}

    # Analyze differences
    diff_file = f"{basename_escaped}.raw"
    diff_lines = get_diff_lines(
        merged_no_comments,
        raw_conflict_no_comments,
        merge_scenario_dir,
        diff_file,
    )
    diff_line_count = analyze_diff_lines(diff_lines, merge_scenario_dir, f"{diff_file}.processed")

    return {
        "file_path": conflict_source.file_path,
        "merged_line_count": conflict_source.merged_line_count,
        "raw_conflict_line_count": count_lines(raw_conflict_no_comments),
        "diff_line_count": diff_line_count,
    }


def add_raw_conflicts_for_project(outdir: str, project: str) -> None:
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
        left_commit, right_commit, base_commit = merge_scenario_parts
        repo_path = join(SAMPLE_BASE, project, project)

        # Validate repo_path is a valid git repo
        try:
            get_repo(repo_path)
        except Exception as e:
            log.error(f"Failed to get git repository at {repo_path}: {e}")
            continue

        # Abort any ongoing merge and clean up
        clean_git_repo(repo_path)

        # Perform git operations
        try:
            git_checkout(repo_path, left_commit)
            git_merge(repo_path, right_commit)
        except Exception as e:
            log.error(f"Git operations failed for {repo_path}: {e}")
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
                conflict_source,
                repo_path,
                merge_scenario_dir,
                project,
                merge_scenario,
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
        total_raw_conflict_line_count = sum(
            cs.get("raw_conflict_line_count", 0) for cs in conflict_source_list
        )

        precision = (
            (total_raw_conflict_line_count - total_diff_line_count)
            / total_raw_conflict_line_count
            if total_raw_conflict_line_count > 0
            else None
        )
        recall = (
            (total_raw_conflict_line_count - total_diff_line_count)
            / total_merged_line_count
            if total_merged_line_count > 0
            else None
        )

        raw_conflict_statistics = {
            "conflict_sources": conflict_source_list,
            "total_diff_line_count": total_diff_line_count,
            "total_merged_line_count": total_merged_line_count,
            "total_raw_conflict_line_count": total_raw_conflict_line_count,
            "precision": precision,
            "recall": recall,
        }

        # Write statistics to file
        raw_conflict_stats_file = join(
            merge_scenario_dir, "raw_conflict_statistics.json"
        )
        log.info(f"mergebot diff lines: {mergebot_stats.total_diff_line_count}, raw diff lines: {raw_conflict_statistics["total_diff_line_count"]}")
        try:
            with open(raw_conflict_stats_file, "w") as f:
                json.dump(raw_conflict_statistics, f, indent=4)
        except Exception as e:
            log.error(f"Failed to write {raw_conflict_stats_file}: {e}")


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
            add_raw_conflicts_for_project(output_dir, project_name)
        else:
            log.error(f"Project '{project_name}' does not exist in {output_dir}")
            sys.exit(1)
    else:
        for project in os.listdir(output_dir):
            project_dir = join(output_dir, project)
            if os.path.isdir(project_dir):
                add_raw_conflicts_for_project(output_dir, project)


if __name__ == "__main__":
    main()

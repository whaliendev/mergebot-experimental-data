import json
import os
import sys
from os.path import join
from typing import List

from octok.model import EvaluateDecoder, MergeScenarioStatistics


def aggeragate_statistics(projects, out_dir):
    for project in projects:
        project_dir = join(out_dir, project)
        if not os.path.isdir(project_dir):
            continue

        merge_scenarios = os.listdir(project_dir)
        stats: List[MergeScenarioStatistics] = []
        raw_stats: List[dict] = []
        mergegen_stats = []
        for merge_scenario in merge_scenarios:
            try:
                merge_scenario_stat_file = join(
                    project_dir, merge_scenario, "statistics.json"
                )

                raw_conflicts_stat_file = join(
                    project_dir, merge_scenario, "raw_conflict_statistics.json"
                )

                mergegen_stat_file = join(
                    project_dir, merge_scenario, "mergegen_conflict_statistics.json"
                )
                if not os.path.isfile(merge_scenario_stat_file) or not os.path.isfile(raw_conflicts_stat_file) or not os.path.isfile(mergegen_stat_file):
                    continue

                with open(merge_scenario_stat_file, "r") as f:
                    merge_scenario_stat: MergeScenarioStatistics = json.load(
                        f, cls=EvaluateDecoder
                    )

                stats.append(merge_scenario_stat)

                with open(raw_conflicts_stat_file, "r") as f:
                    raw_conflicts_stat: dict = json.load(f)
                raw_stats.append(raw_conflicts_stat)

                with open(mergegen_stat_file, "r") as f:
                    mergegen_stat: dict = json.load(f)
                mergegen_stats.append(mergegen_stat)
            except Exception as e:
                print(f'project: {project}, merge_scenario: {merge_scenario}, error: {e}')
                continue

        total_diff_lines = sum([stat.total_diff_line_count for stat in stats])
        total_merged_lines = sum([stat.total_merged_line_count for stat in stats])
        total_mergebot_lines = sum([stat.total_mergebot_line_count for stat in stats])

        precision = (total_mergebot_lines - total_diff_lines) / total_mergebot_lines
        recall = (total_mergebot_lines - total_diff_lines) / total_merged_lines

        print(
            f"project: {project}, precision: {precision}, recall: {recall}, total_diff_lines: {total_diff_lines}, total_merged_lines: {total_merged_lines}, total_mergebot_lines: {total_mergebot_lines}"
        )

        total_raw_diff_lines = sum([stat['total_diff_line_count'] for stat in raw_stats])
        total_raw_merged_lines = sum([stat['total_merged_line_count'] for stat in raw_stats])
        total_raw_conflict_lines = sum([stat['total_raw_conflict_line_count'] for stat in raw_stats])
        precision_raw = (total_raw_conflict_lines - total_raw_diff_lines) / total_raw_conflict_lines
        recall_raw = (total_raw_conflict_lines - total_raw_diff_lines) / total_raw_merged_lines
        print(
            f"project: {project}, precision_raw: {precision_raw}, recall_raw: {recall_raw}, total_raw_diff_lines: {total_raw_diff_lines}, total_raw_merged_lines: {total_raw_merged_lines}, total_raw_conflict_lines: {total_raw_conflict_lines}"
        )

        total_mergegen_diff_lines = sum([stat['total_diff_line_count'] for stat in mergegen_stats])
        total_mergegen_merged_lines = sum([stat['total_merged_line_count'] for stat in mergegen_stats])
        total_mergegen_conflict_lines = sum([stat['total_mergegen_conflict_line_count'] for stat in mergegen_stats])
        precision_mergegen = (total_mergegen_conflict_lines - total_mergegen_diff_lines) / total_mergegen_conflict_lines
        recall_mergegen = (total_mergegen_conflict_lines - total_mergegen_diff_lines) / total_mergegen_merged_lines
        print(
            f"project: {project}, precision_mergegen: {precision_mergegen}, recall_mergegen: {recall_mergegen}, total_mergegen_diff_lines: {total_mergegen_diff_lines}, total_mergegen_merged_lines: {total_mergegen_merged_lines}, total_mergegen_conflict_lines: {total_mergegen_conflict_lines}"
        )
        print()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: aggregator.py <out_dir>")
        sys.exit(1)

    out_dir = sys.argv[1]

    # all directories in out_dir are git projects
    projects = os.listdir(out_dir)

    aggeragate_statistics(projects, out_dir)

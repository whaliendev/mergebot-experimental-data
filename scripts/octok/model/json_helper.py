import json
from .merge_scenario_statistics import MergeScenarioStatistics
from .conflict_source import ConflictSource


class EvaluateEncoder(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, MergeScenarioStatistics):
            return {
                "conflict_sources": o.conflict_sources,
                "total_diff_line_count": o.total_diff_line_count,
                "total_merged_line_count": o.total_merged_line_count,
                "total_mergebot_line_count": o.total_mergebot_line_count,
                "precision": o.precision(),
                "recall": o.recall(),
            }
        elif isinstance(o, ConflictSource):
            return {
                "file_path": o.file_path,
                "merged_line_count": o.merged_line_count,
                "mergebot_line_count": o.mergebot_line_count,
                "diff_line_count": o.diff_line_count,
            }
        elif isinstance(o, MergeScenario):
            return {
                "merged": o.merged,
                "ours": o.ours,
                "theirs": o.theirs,
                "base": o.base,
                "files": o.files,
            }
        else:
            return super().default(o)


def object_hook(obj):
    if "conflict_sources" in obj:
        return MergeScenarioStatistics(
            conflict_sources=obj["conflict_sources"],
            total_diff_line_count=obj["total_diff_line_count"],
            total_merged_line_count=obj["total_merged_line_count"],
            total_mergebot_line_count=obj["total_mergebot_line_count"],
        )
    elif "file_path" in obj:
        return ConflictSource(
            file_path=obj["file_path"],
            merged_line_count=obj["merged_line_count"],
            mergebot_line_count=obj["mergebot_line_count"],
            diff_line_count=obj["diff_line_count"],
        )
    elif "merged" in obj:
        return MergeScenario(
            merged=obj["merged"],
            ours=obj["ours"],
            theirs=obj["theirs"],
            base=obj["base"],
            files=obj["files"],
        )
    return obj


class EvaluateDecoder(json.JSONDecoder):
    def __init__(self, *args, **kwargs):
        super().__init__(object_hook=object_hook, *args, **kwargs)
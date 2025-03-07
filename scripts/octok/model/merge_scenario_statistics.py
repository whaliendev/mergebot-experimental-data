from typing import List
from .conflict_source import ConflictSource

class MergeScenarioStatistics:
    conflict_sources: List[ConflictSource]

    total_diff_line_count: int
    total_merged_line_count: int
    total_mergebot_line_count: int

    def __init__(
        self,
        conflict_sources,
        total_diff_line_count,
        total_merged_line_count,
        total_mergebot_line_count,
    ) -> None:
        self.conflict_sources = conflict_sources
        self.total_diff_line_count = total_diff_line_count
        self.total_merged_line_count = total_merged_line_count
        self.total_mergebot_line_count = total_mergebot_line_count

    def precision(self):
        return (
            self.total_mergebot_line_count - self.total_diff_line_count
        ) / self.total_mergebot_line_count if self.total_mergebot_line_count > 0 else "na"

    def recall(self):
        return (
            self.total_mergebot_line_count - self.total_diff_line_count
        ) / self.total_merged_line_count if self.total_merged_line_count > 0 else "na"

    def __repr__(self) -> str:
        return f"MergeScenarioStatistics(conflict_sources={self.conflict_sources}, total_diff_line_count={self.total_diff_line_count}, total_merged_line_count={self.total_merged_line_count}, total_mergebot_line_count={self.total_mergebot_line_count}, precision={self.precision()}, recall={self.recall()}"
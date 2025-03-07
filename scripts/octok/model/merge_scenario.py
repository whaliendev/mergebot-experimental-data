from typing import List

class MergeScenario:
    merged: str
    ours: str
    theirs: str
    base: str
    files: List[str]

    def __init__(self, merged, ours, theirs, base) -> None:
        self.merged = merged
        self.ours = ours
        self.theirs = theirs
        self.base = base
        self.files = []

    def __repr__(self):
        return f"MergeScenario(merged={self.merged}, ours={self.ours}, theirs={self.theirs}, base={self.base}, files={self.files})"

    def id(self):
        """ours-theirs-base"""
        return f"{self.ours[:8]}-{self.theirs[:8]}-{self.base[:8]}"
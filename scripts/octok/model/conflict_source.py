class ConflictSource:
    file_path: str
    merged_line_count: int
    mergebot_line_count: int
    diff_line_count: int

    def __init__(
        self, file_path, merged_line_count, mergebot_line_count, diff_line_count
    ) -> None:
        self.file_path = file_path
        self.merged_line_count = merged_line_count
        self.mergebot_line_count = mergebot_line_count
        self.diff_line_count = diff_line_count

    def __repr__(self):
        return f"ConflictSource(file_path={self.file_path}, merged_line_count={self.merged_line_count}, mergebot_line_count={self.mergebot_line_count}, diff_line_count={self.diff_line_count})"
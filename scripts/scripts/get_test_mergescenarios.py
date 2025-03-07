import json
import os
import subprocess
from pathlib import Path
from os.path import join


from octok import log
from octok.utils.gitservice import get_repo

log = log.get_logger(__name__)

def get_merge_commits(repo: 'pygit2.Repository', left_commit: str, right_commit: str) -> list:
    """
    Get merge commits that have the specified left and right parents.

    Args:
        repo: The pygit2 Repository object.
        left_commit: The left parent commit hash.
        right_commit: The right parent commit hash.

    Returns:
        A list of matching merge commit lines.
    """
    repo_dir = repo.workdir  # 获取仓库工作目录

    # 构建命令
    command = f"git rev-list --all --merges --parents | grep '{left_commit}.* {right_commit}.*'"

    try:
        # 执行命令
        result = subprocess.run(command, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, shell=True, check=True)

        # 返回结果行
        return result.stdout.splitlines()

    except subprocess.CalledProcessError as e:
        log.error(f"Error executing git command: {e.stderr}")
        return []

def get_test_merge_scenarios(results_dir: str, repo_dir: str = "/home/whalien/sample-repos") -> dict:
    ret = {}
    result_repos = os.listdir(results_dir)
    assert len(result_repos) > 0, f"No repositories found in {results_dir}"
    assert "art" in result_repos, f"Repository 'art' not found in {results_dir}"

    for repo in result_repos:
        git_repo_dir = Path(join(repo_dir, repo, repo)).resolve(strict=True)
        try:
            git_repo = get_repo(str(git_repo_dir))
        except Exception as e:
            log.error(f"Failed to get repository {git_repo_dir}: {e}")
            continue

        ms_dirs = os.listdir(join(results_dir, repo))
        for ms in ms_dirs:
            ours, theirs, base = ms.split("-")
            assert all([ours, theirs, base]), f"Invalid merge scenario directory: {ms}"

            ms_lines = get_merge_commits(git_repo, ours, theirs)
            assert len(ms_lines) == 1, f"Expected 1 merge commit, got {len(ms_lines)}"

            merge_scenarios = ms_lines[0].split()
            assert len(merge_scenarios) == 3, f"Expected 3 commit hash, got {len(merge_scenarios)}"

            ret.setdefault(repo, []).append(merge_scenarios[0])

        log.info(f"{repo} processed successfully, {len(ret[repo])} merge commits found")

    return ret


if __name__ == "__main__":
    results_dir = "./output"
    taboo_dict = get_test_merge_scenarios(results_dir)

    with open("taboo.json", "w") as f:
        f.write(json.dumps(taboo_dict, indent=4))
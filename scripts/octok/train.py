#!/bin/env python3
import argparse
import json
import logging
import multiprocessing
import os
import subprocess
import sys
from logging import Manager
from multiprocessing import Value, Lock
from pathlib import Path
from typing import List

import pygit2

from octok.utils.gitservice import get_repo, process_conflict_blocks

err_no = 0
MAX_MERGE_SCENARIOS_PER_PROJECT = 1000
MAX_CONFLICTING_FILES = 6600

with open("./taboo.json", "r") as f:
    taboo_dict = json.load(f)


def setup_logger(log_filename="app.log"):
    # Create logs directory if it doesn't exist
    log_dir = "logs"
    os.makedirs(log_dir, exist_ok=True)

    # Set up the logger
    logger = logging.getLogger(__name__)
    logger.setLevel(logging.DEBUG)

    # Create file handler
    file_handler = logging.FileHandler(os.path.join(log_dir, log_filename))
    file_handler.setLevel(logging.DEBUG)

    # Create console handler
    console_handler = logging.StreamHandler(sys.stdout)
    console_handler.setLevel(logging.INFO)  # Adjust level as needed

    # Create a custom formatter to include the process ID
    formatter = logging.Formatter('%(asctime)s - PID: %(process)d - %(levelname)s - %(message)s')
    file_handler.setFormatter(formatter)
    console_handler.setFormatter(formatter)

    # Add handlers to the logger
    logger.addHandler(file_handler)
    logger.addHandler(console_handler)

    return logger

log = setup_logger()


def parse_arguments():
    parser = argparse.ArgumentParser(description="Git Merge Conflict Collector")
    parser.add_argument('--git_repo_dir', type=str, help='Input directory containing git repositories')
    parser.add_argument('--output_dir', type=str, help='Output directory to store conflict files')
    parser.add_argument('--processes', type=int, default=4, help='Number of parallel processes')
    return parser.parse_args()


def get_merge_base(repo, commit_a, commit_b):
    try:
        base = repo.merge_base(commit_a.id, commit_b.id)
        if base is not None:
            return repo.get(base)
        else:
            return None
    except Exception as e:
        log.warning(f"Failed to find merge base for commits {commit_a.id} and {commit_b.id}: {e}")
        return None


def perform_merge(repo, commit_a, commit_b, base):
    try:
        index = repo.merge_commits(commit_a, commit_b, flags={"fail_on_conflict": False})
        if index.conflicts:
            return index.conflicts
        else:
            return None
    except Exception as e:
        log.error(f"Merge failed between {commit_a.id} and {commit_b.id}: {e}")
        return None


def is_binary_string(bytes_data):
    """
    Check if the given bytes data is binary.
    """
    textchars = bytearray({7, 8, 9, 10, 12, 13, 27}
                          | set(range(0x20, 0x100)) - {0x7f})
    return bool(bytes_data.translate(None, textchars))


def get_file_content(repo, commit, file_path):
    try:
        tree = commit.tree
        blob = tree[file_path].data
        if is_binary_string(blob):
            log.debug(f"skipping binary file: {file_path} in commit {commit.id}")
            return None  # Indicate binary file
        content = blob.decode('utf-8', errors='replace')
        return content
    except KeyError:
        return ""
    except Exception as e:
        log.warning(f"failed to get content for file {file_path} in commit {commit.id}: {e}")
        return ""


def perform_3_way_merge(a_content: str, b_content: str, base_content: str, tmp_dir_name: str) -> str:
    tmp_dir = os.path.join("/tmp", tmp_dir_name)
    os.makedirs(tmp_dir, exist_ok=True)

    # Define temporary file paths
    a_path = os.path.join(tmp_dir, 'a')
    b_path = os.path.join(tmp_dir, 'b')
    base_path = os.path.join(tmp_dir, 'base')
    merge_path = os.path.join(tmp_dir, 'merge')

    # Write the contents to temporary files
    with open(a_path, 'w') as a_file:
        a_file.write(a_content)

    with open(b_path, 'w') as b_file:
        b_file.write(b_content)

    with open(base_path, 'w') as base_file:
        base_file.write(base_content)

    # Execute the git merge-file command
    try:
        subprocess.run(
            ['git', 'merge-file', '-L', 'a', '-L', 'base', '-L', 'b', a_path, base_path, b_path, '--diff3', '-p'],
            stdout=open(merge_path, 'w')
        )
    except subprocess.CalledProcessError as e:
        raise RuntimeError("Merge failed") from e

    # Read the merged content from the file
    with open(merge_path, 'r') as merge_file:
        merged_content = merge_file.read()

    return merged_content


def get_leading_context(conflicting_lines: List[str], start_line: int, context_size: int = 10) -> str:
    """
    Get the leading context of the conflict block.
    """
    start_line = max(0, start_line - context_size + 1)
    return "\n".join(conflicting_lines[start_line: start_line + context_size])


def get_trainling_context(conflicting_lines: List[str], end_line: int, context_size: int = 10) -> str:
    """
    Get the trailing context of the conflict block.
    """
    return "\n".join(conflicting_lines[end_line: end_line + context_size])


def dump_conflict_file(a_content: str, b_content: str, base_content: str, resolved_content: str, merged_content: str,
                       file_meta: dict, merge_scenario_dir: str, file_index: int):
    """
    Dump the conflict file to the output directory.
    """
    try:
        file_ext = Path(file_meta["fname"]).suffix
        a_file_path = os.path.join(merge_scenario_dir, f"{file_index}_a{file_ext}")
        b_file_path = os.path.join(merge_scenario_dir, f"{file_index}_b{file_ext}")
        base_file_path = os.path.join(merge_scenario_dir, f"{file_index}_base{file_ext}")
        resolved_file_path = os.path.join(merge_scenario_dir, f"{file_index}_resolved{file_ext}")
        merged_file_path = os.path.join(merge_scenario_dir, f"{file_index}_merged{file_ext}")
        metadata_file_path = os.path.join(merge_scenario_dir, f"{file_index}_metadata.json")

        with (open(a_file_path, 'w') as afile,
              open(b_file_path, 'w') as bfile,
              open(base_file_path, 'w') as basefile,
              open(resolved_file_path, 'w') as resolvedfile,
              open(merged_file_path, 'w') as mergedfile,
              open(metadata_file_path, 'w') as metadatafile):
            afile.write(a_content)
            bfile.write(b_content)
            basefile.write(base_content)
            resolvedfile.write(resolved_content)
            mergedfile.write(merged_content)
            json.dump(file_meta, metadatafile, indent = 2)
            return True
    except Exception as e:
        log.error(f"failed to dump conflict file: {e}")
        # clean up
        os.removedirs(merge_scenario_dir)
        return False


def process_repository(repo: str, git_repo_path: str, output_dir: str, file_index: multiprocessing.Value, lock: multiprocessing.Lock):
    log.info(f"processing repository {repo}")

    repo_dir = Path(git_repo_path, repo).resolve(strict=True)
    output_repo_dir = Path(output_dir, repo).resolve()
    os.makedirs(output_repo_dir, exist_ok=True)

    try:
        git_repo = get_repo(str(repo_dir))
    except Exception as e:
        log.error(f"failed to get repository {repo_dir}: {e}")
        return

    try:
        current_branch = git_repo.head.shorthand
        log.info(f"current branch: {current_branch}")
    except Exception as e:
        log.error(f"failed to get current branch: {e} for repo {repo_dir}")
        return

    try:
        walker = git_repo.walk(git_repo.head.target, pygit2.GIT_SORT_TIME)
    except Exception as e:
        log.error(f"failed to get walker: {e} for repo {repo_dir}")
        return

    for commit in walker:
        if len(commit.parents) != 2:
            continue

        if repo in taboo_dict and str(commit.hex) in taboo_dict[repo]:
            continue

        parent_a, parent_b = commit.parents
        merge_base = get_merge_base(git_repo, parent_a, parent_b)

        if not merge_base:
            continue  # cannot find merge base

        conflicts = perform_merge(git_repo, parent_a, parent_b, merge_base)
        if not conflicts:
            continue  # no conflicts

        merge_scenario_dir = output_repo_dir / str(commit.hex)
        os.makedirs(merge_scenario_dir, exist_ok=True)
        for conflict in conflicts:
            # conflict is a 3-tuple (ancestor, ours, theirs), every entry is a IndexEntry
            if not conflict[0]:
                continue

            file_path = conflict[0].path
            suffix = Path(file_path).suffix
            if suffix not in ['.c', '.cpp', '.cxx', '.C', '.cx', '.h', '.hpp', 'hxx', '.H', '.inl']:
                continue

            a_content = get_file_content(repo, parent_a, file_path)
            if not a_content:
                log.info(f"uninteresting file: {file_path}, a_content is empty")
                continue

            b_content = get_file_content(repo, parent_b, file_path)
            if not b_content:
                log.info(f"uninteresting file: {file_path}, b_content is empty")
                continue

            base_content = get_file_content(repo, merge_base, file_path)
            if not base_content:
                log.info(f"uninteresting file: {file_path}, base_content is empty")
                continue

            resolved_content = get_file_content(repo, commit, file_path)
            if not resolved_content:
                log.info(f"uninteresting file: {file_path}, resolved_content is empty")
                continue

            # get merged content
            merged_content = perform_3_way_merge(a_content, b_content, base_content, f"{repo}-{commit.hex}")
            resolved_lines = resolved_content.splitlines()
            conflict_blocks = process_conflict_blocks(merged_content, resolved_content)

            with lock:
                if file_index.value >= MAX_CONFLICTING_FILES:
                    log.info(f"maximum number of conflicting files reached for repo {repo}")
                    return

                conflicting_chunks = [{
                    "a_contents": conflict_block.ours,
                    "b_contents": conflict_block.theirs,
                    "base_contents": conflict_block.base,
                    "res_region": conflict_block.merged,
                    "lookback": get_leading_context(resolved_lines, conflict_block.resolved_start_line),
                    "lookahead": get_trainling_context(resolved_lines, conflict_block.resolved_end_line),
                    "label": conflict_block.labels
                } for conflict_block in conflict_blocks]
                file_meta = {
                    "fname": file_path,
                    "repo": repo,
                    "commitHash": str(commit.hex),
                    "conflicting_chunks": conflicting_chunks
                }

                success = dump_conflict_file(a_content, b_content, base_content, resolved_content, merged_content,
                                             file_meta,
                                             str(merge_scenario_dir), file_index.value)

                if success:
                    file_index.value += 1
                    log.info(f"conflict file {file_path} dumped successfully, file index now comes to {file_index.value}")

if __name__ == "__main__":
    args = parse_arguments()
    git_repo_dir = args.git_repo_dir
    output_dir = args.output_dir
    processes_num = args.processes

    git_repo_path = Path(git_repo_dir).resolve(strict=True)
    os.makedirs(output_dir, exist_ok=True)

    # for repo in os.listdir(git_repo_path):
    #     process_repository(repo, git_repo_path, output_dir)
    with multiprocessing.Manager() as manager:
        file_index = manager.Value('i', 0)
        lock = manager.Lock()
        with multiprocessing.Pool(processes=processes_num) as pool:
            pool.starmap(process_repository, [(repo, git_repo_path, output_dir, file_index, lock) for repo in os.listdir(git_repo_path)])


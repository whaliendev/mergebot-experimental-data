import os
import json
import sys


def find_compile_commands(root_dir):
    """
    Walk through the directory tree starting at root_dir.
    For each directory containing a compile_commands.json file,
    load the JSON content and record the number of entries.

    Args:
        root_dir (str): The root directory to start the search.

    Returns:
        list of tuples: Each tuple contains the directory path and the number of entries.
    """
    global_list = []

    for current_dir, dirs, files in os.walk(root_dir, topdown=True):
        if "compile_commands.json" in files:
            compile_commands_path = os.path.join(current_dir, "compile_commands.json")
            try:
                with open(compile_commands_path, "r", encoding="utf-8") as f:
                    data = json.load(f)

                if isinstance(data, list):
                    entry_count = len(data)
                    absolute_path = os.path.abspath(current_dir)
                    global_list.append((absolute_path, entry_count))
                else:
                    print(
                        f"Warning: {compile_commands_path} does not contain a JSON array.",
                        file=sys.stderr,
                    )

            except json.JSONDecodeError as jde:
                print(
                    f"Error: Failed to parse JSON in {compile_commands_path}: {jde}",
                    file=sys.stderr,
                )
            except Exception as e:
                print(
                    f"Error: Unable to read {compile_commands_path}: {e}",
                    file=sys.stderr,
                )

    return global_list


def main():
    """
    Main function to execute the script.
    """
    # Define the root directory; default is the current directory.
    root_directory = "."

    # Optional: Allow the user to specify a different root directory via command-line arguments.
    if len(sys.argv) > 1:
        root_directory = sys.argv[1]
        if not os.path.isdir(root_directory):
            print(
                f"Error: The specified path '{root_directory}' is not a directory or does not exist.",
                file=sys.stderr,
            )
            sys.exit(1)

    # Find and process all compile_commands.json files.
    compile_commands_list = find_compile_commands(root_directory)

    # Sort the global list by the number of entries in descending order.
    sorted_list = sorted(compile_commands_list, key=lambda x: x[1], reverse=True)

    # Print the sorted list.
    print(f"{'Directory Path':<80} | {'Number of Entries'}")
    print("-" * 100)
    for directory, count in sorted_list:
        print(f"{directory:<80} | {count}")


if __name__ == "__main__":
    main()

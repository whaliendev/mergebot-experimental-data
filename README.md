### Current Data
The following data table is aggregated from `statistics.json` in the repo: 
| project           | precision | recall | total_diff_lines | total_merged_lines | total_mergebot_lines |
| ----------------- | --------: | -----: | ---------------: | -----------------: | -------------------: |
| duckdb            |    52.77% | 88.08% |            17246 |              21876 |                36515 |
| frameworks_av     |    66.89% | 52.29% |            11599 |              44813 |                35030 |
| libnativehelper   |    89.14% | 92.29% |              105 |                934 |                  967 |
| flameshot         |    97.94% | 98.76% |              112 |               5394 |                 5439 |
| tmux              |    95.72% | 88.58% |             1785 |              45097 |                41734 |
| redis             |    93.46% | 98.72% |             5394 |              78076 |                82473 |
| frameworks_native |    70.48% | 65.70% |            22509 |              81794 |                76245 |
| rocksdb           |    85.06% | 87.31% |            13516 |              88134 |                90464 |
| grpc              |    50.40% | 68.08% |            20042 |              29907 |                40404 |
| art               |    63.15% | 77.62% |            23900 |              52765 |                64854 |

We define Precision and Recall as follows:

$$
Precision = \frac{total\_mergebot\_lines - total\_diff\_lines}{total\_mergebot\_lines}
$$

$$
Recall = \frac{total\_mergebot\_lines - total\_diff\_lines}{total\_merged\_lines}
$$

Based on above data table from 10 C/C++ projects, the average precision is 75.49% and the average recall is 79.75%.
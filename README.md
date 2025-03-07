## mergebot-experimental-data

The experimental data of MergeBot platform.

### Directory Structure

```shell
.
├── raw-data/     # Original merge conflict datasets
├── README.md     # README
├── scripts/      # Data processing/analysis utilities
└── summary/      # Processed results
```

#### Key Contents

• **`raw-data/`**  
 Contains original merge conflict scenarios, with metadata including project sources.

• **`scripts/`**  
 python tools for:  
 • Conflict extraction from git history  
 • Metrics calculation (resolution accuracy, time efficiency)  
 • Statistical analysis scripts

• **`summary/`**  
 Final processed datasets (CSV):  
 • Comparative tables  
 • The heuristic results


import pandas as pd
import glob
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
data_dir = os.path.join(script_dir, "..", "data")

# Create an empty list to store DataFrames
dfs = []

# Read all pingpong summary CSVs
for path in glob.glob(os.path.join(data_dir, "pingpong_*summary.csv")):
    df = pd.read_csv(path)
    dfs.append(df)

if dfs:
    # Concatenate all DataFrames
    all_data = pd.concat(dfs, ignore_index=True)
    
    # Filter for size 64 and 4096 (the sizes typically used in the tables) as well as 65536 (64 KiB) and 1048576 (1 MiB) for Threat 7
    filtered = all_data[all_data['message_size_bytes'].isin([64, 4096, 65536, 1048576])]
    
    # Sort for consistent output
    sorted_data = filtered.sort_values(by=['message_size_bytes', 'ipc_type', 'wakeup_variant'])
    
    # Select columns to match Table V requirements
    columns_to_keep = [
        'ipc_type', 'wakeup_variant', 'message_size_bytes', 
        'mean_us', 'median_us', 'p90_us', 'p99_us', 'p999_us', 
        'ci95_lo_us', 'ci95_hi_us'
    ]
    final_data = sorted_data[columns_to_keep]
    
    # Rename for readability
    final_data.columns = [
        'IPC Type', 'Wakeup', 'Size (B)', 'Mean', 'P50', 'P90', 'P99', 'P99.9', 'CI 95% Lo', 'CI 95% Hi'
    ]
    
    # Save to CSV
    output_path = os.path.join(data_dir, "table_v_full_percentiles.csv")
    final_data.to_csv(output_path, index=False)
    print(f"Saved {output_path}")
    print(final_data.to_string(index=False))
else:
    print("No data files found.")

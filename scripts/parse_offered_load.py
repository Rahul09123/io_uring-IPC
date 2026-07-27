import pandas as pd
import numpy as np

import os

script_dir = os.path.dirname(os.path.abspath(__file__))
data_dir = os.path.join(script_dir, "..", "data")

try:
    df = pd.read_csv(os.path.join(data_dir, "ablation_results.csv"))
    
    # Filter for size 64B
    df_64 = df[df['message_size_bytes'] == 64].copy()
    
    # Pivot for throughput
    pt_tp = df_64.pivot_table(index='wakeup_variant', columns='regime', values='throughput_gbps', aggfunc='mean')
    
    # Pivot for median wakeup latency
    pt_lat = df_64.pivot_table(index='wakeup_variant', columns='regime', values='wakeup_latency_p50_us', aggfunc='mean')
    
    # Define column order
    cols = ['offered_25', 'offered_50', 'offered_75', 'offered_90', 'saturated']
    
    print("--- 64B Throughput (GiB/s) ---")
    if set(cols).issubset(pt_tp.columns):
        print(pt_tp[cols].round(2))
    else:
        print(pt_tp)
        
    print("\n--- 64B Median Wakeup Latency (us) ---")
    if set(cols).issubset(pt_lat.columns):
        print(pt_lat[cols].round(2))
    else:
        print(pt_lat)

    # Save to CSV
    output_path = os.path.join(data_dir, "offered_load_summary.csv")
    
    # Combine into one table with multi-level columns
    combined = pd.concat([pt_tp, pt_lat], keys=['Throughput (GiB/s)', 'Wakeup Latency P50 (us)'], axis=1)
    combined.to_csv(output_path)
    print(f"\nSaved combined data to {output_path}")

except Exception as e:
    print(f"Error: {e}. Ensure ablation_results.csv exists (run run_ablation.sh).")

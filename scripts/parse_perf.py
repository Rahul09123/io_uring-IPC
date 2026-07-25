import re
import glob
import pandas as pd
import os

def parse_perf_stat(path):
    text = open(path).read()
    def grab(pattern):
        m = re.search(pattern, text)
        return float(m.group(1).replace(",", "")) if m else None

    return {
        "cycles": grab(r"([\d,]+)\s+cycles"),
        "instructions": grab(r"([\d,]+)\s+instructions"),
        "context_switches": grab(r"([\d,]+)\s+context-switches"),
        "cpu_migrations": grab(r"([\d,]+)\s+cpu-migrations"),
        "task_clock_ms": grab(r"([\d.]+)\s+task-clock"),
    }

rows = []
# The ablation test uses size 64B, bursty regime.
# Producer sends NUM_RUNS (100) * NUM_MESSAGES_PER_RUN (e.g. 100,000) ? 
# Let's check how many messages the ablation producer sends in bursty mode.
# Table VI shows 10M messages total typically. We will assume 10,000,000 messages or we can read it.
# Wait, let's look at `common.h` in ablation to see `NUM_RUNS` and how much data is sent.
# I will use a placeholder of 1,000,000 for now and fix it if needed.

n_messages = 4_194_304

script_dir = os.path.dirname(os.path.abspath(__file__))
data_dir = os.path.join(script_dir, "..", "data")

for path in glob.glob(os.path.join(data_dir, "perf_*_bursty_64B.txt")):
    m = re.match(r"perf_(.+)_bursty_64B\.txt", os.path.basename(path))
    if not m:
        continue
    variant = m.group(1)
    
    stats = parse_perf_stat(path)
    if not stats["cycles"]:
        continue
        
    cycles_per_msg = stats["cycles"] / n_messages
    ctxsw_per_msg = stats["context_switches"] / n_messages
    insn_per_msg = stats["instructions"] / n_messages
    
    rows.append({
        "Variant": variant,
        "Size": "64B",
        "Cycles/msg": cycles_per_msg,
        "Ctx-sw/msg": ctxsw_per_msg,
        "Insn/msg": insn_per_msg
    })

if rows:
    df = pd.DataFrame(rows)
    df = df.sort_values(by="Cycles/msg")
    output_path = os.path.join(data_dir, "cpu_cost_per_variant.csv")
    df.to_csv(output_path, index=False)
    print(f"Saved {output_path}")
    print(df.to_string(index=False))
else:
    print("No perf files found.")

#!/bin/bash

# Copyright (c) 2025 All-Hands
# All rights reserved.
#
# This script runs the prefetcher comparison tests with different benchmarks

# Set the base directory for results
RESULTS_DIR="prefetcher_comparison_results"
mkdir -p $RESULTS_DIR

# SPEC CPU benchmarks to test (assuming they are installed)
BENCHMARKS=(
    "/bin/ls"  # Simple command for testing
    "/bin/grep"  # Another simple command for testing
)

# Command line arguments for each benchmark
BENCHMARK_ARGS=(
    "-la /"
    "-r \"test\" /etc"
)

# Run the comparison for each benchmark
for i in "${!BENCHMARKS[@]}"; do
    BENCHMARK=${BENCHMARKS[$i]}
    ARGS=${BENCHMARK_ARGS[$i]}
    BENCHMARK_NAME=$(basename $BENCHMARK)
    
    echo "Running prefetcher comparison for $BENCHMARK_NAME..."
    
    # Create a directory for this benchmark's results
    BENCHMARK_DIR="$RESULTS_DIR/$BENCHMARK_NAME"
    mkdir -p $BENCHMARK_DIR
    
    # Run the comparison script
    ./build/X86/gem5.opt configs/example/prefetcher_comparison.py \
        --cmd=$BENCHMARK \
        --options="$ARGS" \
        --output-dir=$BENCHMARK_DIR \
        --max-insts=10000000 \
        --stat-period=1000000 \
        --l1i-size=32kB \
        --l1d-size=32kB \
        --l2-size=1MB \
        --l2-assoc=8 \
        --piq-size=16 \
        --ftq-size=16 \
        --prefetch-buffer-size=32 \
        --prefetch-degree=4 \
        --prefetch-distance=2
    
    echo "Completed prefetcher comparison for $BENCHMARK_NAME"
    echo "Results saved to $BENCHMARK_DIR"
    echo ""
done

# Generate a summary report
echo "Generating summary report..."
SUMMARY_FILE="$RESULTS_DIR/summary.txt"

echo "Prefetcher Comparison Summary" > $SUMMARY_FILE
echo "============================" >> $SUMMARY_FILE
echo "" >> $SUMMARY_FILE

for i in "${!BENCHMARKS[@]}"; do
    BENCHMARK_NAME=$(basename ${BENCHMARKS[$i]})
    BENCHMARK_DIR="$RESULTS_DIR/$BENCHMARK_NAME"
    CSV_FILE="$BENCHMARK_DIR/prefetcher_comparison.csv"
    
    if [ -f "$CSV_FILE" ]; then
        echo "Results for $BENCHMARK_NAME:" >> $SUMMARY_FILE
        echo "------------------------" >> $SUMMARY_FILE
        
        # Extract and format the data from the CSV file
        echo "Prefetcher | IPC | I$ Miss Rate | L2 Miss Rate" >> $SUMMARY_FILE
        echo "----------|-----|--------------|-------------" >> $SUMMARY_FILE
        
        # Skip the header line and process each row
        tail -n +2 $CSV_FILE | while IFS=, read -r prefetcher sim_ticks sim_seconds ipc icache_miss_rate l2_miss_rate rest; do
            echo "$prefetcher | $ipc | $icache_miss_rate | $l2_miss_rate" >> $SUMMARY_FILE
        done
        
        echo "" >> $SUMMARY_FILE
    fi
done

echo "Summary report generated at $SUMMARY_FILE"
echo "Prefetcher comparison tests completed!"
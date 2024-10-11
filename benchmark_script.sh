#!/bin/bash

set -ex

# Function to print usage
usage() {
  echo "Usage: $0 --storage-path /path/to/storage --bench-name bench_name --runs runs --stopatheight stopatheight --dbcache dbcache --printtoconsole printtoconsole --preseed preseed --crash-interval-seconds crash_interval_seconds --commit-list commit_list"
  exit 1
}

while [[ "$#" -gt 0 ]]; do
  case $1 in
    --storage-path) export STORAGE_PATH="$2"; shift ;;
    --bench-name) export BASE_NAME="$2"; shift ;;
    --runs) export RUNS="$2"; shift ;;
    --stopatheight) export STOP_AT_HEIGHT="$2"; shift ;;
    --dbcache) export DBCACHE="$2"; shift ;;
    --printtoconsole) export PRINT_TO_CONSOLE="$2"; shift ;;
    --preseed) export PRESEED="$2"; shift ;;
    --crash-interval-seconds) export CRASH_INTERVAL_SECONDS="$2"; shift ;;
    --commit-list) export COMMIT_LIST="$2"; shift ;;
    *) echo "Unknown parameter passed: $1"; usage ;;
  esac
  shift
done

if [ -z "$STORAGE_PATH" ] || [ -z "$BASE_NAME" ] || [ -z "$RUNS" ] || [ -z "$STOP_AT_HEIGHT" ] || [ -z "$DBCACHE" ] || [ -z "$PRINT_TO_CONSOLE" ] || [ -z "$PRESEED" ] || [ -z "$CRASH_INTERVAL_SECONDS" ] || [ -z "$COMMIT_LIST" ]; then
  usage
fi

START_DATE=$(date +%Y%m%d%H%M%S)
export DATA_DIR="$STORAGE_PATH/BitcoinData"
mkdir -p "$DATA_DIR"
export PROJECT_DIR="$STORAGE_PATH/${BASE_NAME}_${START_DATE}"
mkdir -p "$PROJECT_DIR"

export LOG_FILE="$PROJECT_DIR/benchmark.log"
export JSON_FILE="$PROJECT_DIR/benchmark.json"

echo "Storage Path: $STORAGE_PATH" | tee -a "$LOG_FILE"
echo "Benchmark Name: $BASE_NAME" | tee -a "$LOG_FILE"
echo "Runs: $RUNS" | tee -a "$LOG_FILE"
echo "Stop at Height: $STOP_AT_HEIGHT" | tee -a "$LOG_FILE"
echo "DB Cache: $DBCACHE" | tee -a "$LOG_FILE"
echo "Print to Console: $PRINT_TO_CONSOLE" | tee -a "$LOG_FILE"
echo "Preseed: $PRESEED" | tee -a "$LOG_FILE"
echo "Crash Interval in Seconds: $CRASH_INTERVAL_SECONDS" | tee -a "$LOG_FILE"
echo "Commit List: $COMMIT_LIST" | tee -a "$LOG_FILE"

prepare_function() {
  echo "Starting prepare step at commit $COMMIT at $(date)" | tee -a "$LOG_FILE"

  killall bitcoind vmstat || true

  git checkout "$COMMIT" || { echo "Failed to checkout commit $COMMIT" | tee -a "$LOG_FILE"; exit 1; }
  COMMIT_MSG=$(git log --format=%B -n 1)
  echo "Preparing commit: $COMMIT: $COMMIT_MSG" | tee -a "$LOG_FILE"

  # Build Bitcoin Core
  cmake -B build -DWITH_ZSTD=ON -DCMAKE_BUILD_TYPE=Release -DBUILD_UTIL=OFF -DBUILD_TX=OFF -DBUILD_TESTS=OFF -DENABLE_WALLET=OFF -DINSTALL_MAN=OFF
  cmake --build build -j$(nproc) || { echo "Build failed at commit $COMMIT" | tee -a "$LOG_FILE"; exit 1; }

  # Cleanup data directory and caches
  rm -rf "$DATA_DIR"/*
  sync
  echo 3 > /proc/sys/vm/drop_caches
  echo "Cleared data directory and dropped caches at commit $COMMIT at $(date)" | tee -a "$LOG_FILE"

  # Preseed bitcoind if option is enabled
  if [ "$PRESEED" = true ]; then
    echo "Starting bitcoind with large dbcache for preseed at commit: $COMMIT: '$COMMIT_MSG'  at $(date)" | tee -a "$LOG_FILE"
    ./build/src/bitcoind -datadir="$DATA_DIR" -dbcache=10000 -stopatheight=1 -printtoconsole=1
    echo "Preseed complete at $(date)" | tee -a "$LOG_FILE"
  fi

  echo "Finished prepare step at commit $COMMIT at $(date)" | tee -a "$LOG_FILE"
}
export -f prepare_function

# Run and crash bitcoind periodically and restart it until it exits successfully
run_and_crash_bitcoind_periodically() {
  local DB_CRASH_RATIO=0
  while true; do
    # Crash bitcoind at intervals if CRASH_INTERVAL_SECONDS is set
    if [[ "$CRASH_INTERVAL_SECONDS" -gt 0 ]]; then
      DB_CRASH_RATIO=2 # 50% chance of crashing

      {
        sleep "$CRASH_INTERVAL_SECONDS"
        echo "Killing bitcoind with SIGKILL after $CRASH_INTERVAL_SECONDS seconds." | tee -a "$LOG_FILE"
        killall -SIGKILL bitcoind || true
      } &
    fi

    echo "Starting bitcoind process with commit $COMMIT at $(date)" | tee -a "$LOG_FILE"
    ./build/src/bitcoind -datadir="$DATA_DIR" -stopatheight="$STOP_AT_HEIGHT" -dbcache="$DBCACHE" -printtoconsole="$PRINT_TO_CONSOLE" -dbcrashratio="$DB_CRASH_RATIO" -maxmempool=5 -blocksonly
    if [ $? -eq 0 ]; then
      echo "bitcoind finished successfully with exit code 0" | tee -a "$LOG_FILE"
      break
    else
      echo "bitcoind crashed, restarting..." | tee -a "$LOG_FILE"
    fi
  done
}
export -f run_and_crash_bitcoind_periodically

run_bitcoind_with_monitoring() {
  echo "run_bitcoind_with_monitoring:" | tee -a "$LOG_FILE"

  COMMIT_MSG=$(git log --format=%B -n 1)
  echo "Measuring commit: $COMMIT: '$COMMIT_MSG'" | tee -a "$LOG_FILE"

  # Start vmstat monitoring
  vmstat 1 > "$PROJECT_DIR/vmstat_${COMMIT}_$(date +%Y%m%d%H%M%S).log" &
  VMSTAT_PID=$!

  run_and_crash_bitcoind_periodically

  echo "VMSTAT monitoring at commit $COMMIT at $(date)" | tee -a "$LOG_FILE"
  vmstat -s | tee -a "$LOG_FILE"
  kill $VMSTAT_PID
}
export -f run_bitcoind_with_monitoring

cleanup_function() {
  echo "cleanup_function:" | tee -a "$LOG_FILE"

  {
    # Log data directory stats
    echo "Data directory size after benchmark at commit $COMMIT: $(du -sh "$DATA_DIR" | cut -f1)"
    echo "Number of files in data directory: $(find "$DATA_DIR" -type f | wc -l)"
  } | tee -a "$LOG_FILE"

  echo "Starting bitcoind for $COMMIT at $(date)" | tee -a "$LOG_FILE"
  ./build/src/bitcoind -daemon -datadir="$DATA_DIR" -dbcache="$DBCACHE" -printtoconsole=0 && sleep 10

  {
    echo "Benchmarking gettxoutsetinfo at $(date)"
    time ./build/src/bitcoin-cli -datadir="$DATA_DIR" gettxoutsetinfo
  } 2>&1 | tee -a "$LOG_FILE"

  echo "Stopping bitcoind for $COMMIT at $(date)" | tee -a "$LOG_FILE"
  ./build/src/bitcoin-cli -datadir="$DATA_DIR" stop && sleep 10
  killall bitcoind vmstat || true

  echo "Ended $COMMIT: $COMMIT_MSG at $(date)" | tee -a "$LOG_FILE"
}
export -f cleanup_function

run_benchmarks() {
  hyperfine \
    --shell=bash \
    --runs "$RUNS" \
    --show-output \
    --export-json "$JSON_FILE" \
    --parameter-list COMMIT "$COMMIT_LIST" \
    --prepare 'COMMIT={COMMIT} prepare_function' \
    --cleanup 'COMMIT={COMMIT} cleanup_function' \
    "COMMIT={COMMIT} run_bitcoind_with_monitoring"
}

# Example usage of the benchmark script with parameter names
# ./benchmark_script.sh \
#  --storage-path /mnt/my_storage \
#  --bench-name rocksdb-bench \
#  --runs 1 \
#  --stopatheight 100000 \
#  --dbcache 2000 \
#  --printtoconsole 0 \
#  --preseed true \
#  --crash-interval-seconds 0 \
#  --commit-list "6c180ad76776d1ef21160daecda72bba94fd07ed,d6fc509508c727c8755933293d8e5d06e4421a16,d7ae58fb0275a258966925ba832491efa1b842a1"

run_benchmarks

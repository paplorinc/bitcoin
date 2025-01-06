set shell := ["bash", "-uc"]

os := os()

default:
    just --list

# Build base and head binaries for CI
[group('ci')]
build-assumeutxo-binaries base_commit head_commit:
    #!/usr/bin/env bash
    set -euxo pipefail
    for build in "base:{{ base_commit }}" "head:{{ head_commit }}"; do
        name="${build%%:*}"
        commit="${build#*:}"
        git checkout "$commit"
        taskset -c 0-15 cmake -B "build-$name" \
            -DBUILD_BENCH=OFF \
            -DBUILD_TESTS=OFF \
            -DBUILD_TX=OFF \
            -DBUILD_UTIL=OFF \
            -DINSTALL_MAN=OFF \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
        taskset -c 0-15 cmake --build "build-$name" -j {{ num_cpus() }}
    done

# Run signet assumeutxo CI workflow
[group('ci')]
run-assumeutxo-signet-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir binaries_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} signet 200000 "148.251.128.115:55555" {{ dbcache }} {{ binaries_dir }}

# Run mainnet assumeutxo CI workflow for default cache
[group('ci')]
run-assumeutxo-mainnet-default-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir binaries_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} main 850000 "148.251.128.115:33333" {{ dbcache }} {{ binaries_dir }}

# Run mainnet assumeutxo CI workflow for large cache
[group('ci')]
run-assumeutxo-mainnet-large-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir binaries_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} main 850000 "148.251.128.115:33333" {{ dbcache }} {{ binaries_dir }}

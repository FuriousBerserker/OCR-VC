#!/bin/bash

set -e

OCR_HOME=/home/berserker/main/workspace/Research/OCRDataRaceDetection/OCRDebugRuntime

APP_HOME=/home/berserker/main/src/ocr-1.1/xstg/apps/apps

LOG_FILE="$PWD/log.txt"
OUTPUT_FILE="$PWD/output.txt"

run_benchmark() {
    #argument
    local iterations=${1}
    local awk_command=$2
    local benchmark_path=$3
    local executable=$4
    local args=${@:5}
    #constant
    local thread_num=1
    local config_path="$PWD/race-detect.cfg"
    local pintool_path=obj-intel64/OCR-VC.so
    local benchmark_command="$benchmark_path/install/x86/$executable"
    export OCR_INSTALL=$OCR_HOME/install
    export LD_LIBRARY_PATH=$OCR_INSTALL/lib:$APP_HOME/libs/install/x86/lib:$LD_LIBRARY_PATH
    export OCR_CONFIG=$config_path
    export CONFIG_NUM_THREADS=$thread_num
    export OCR_TYPE=x86
    export NO_DEBUG=yes
    export CFLAGS="-O3 -DNDEBUG=1"
    export CC=gcc
    export CXX=g++
    echo $executable >> $OUTPUT_FILE
    pushd $benchmark_path > /dev/null
    make -f Makefile.x86 clean install | tee -a $LOG_FILE
    popd > /dev/null
    for ((index = 0; index < iterations; index++)); do
        pin -t "$pintool_path" -- "$benchmark_command" $args | tee -a $LOG_FILE | awk "$awk_command" >> $OUTPUT_FILE
    done

}

run_benchmarks() {
    local experiment_name=$1
    local macro_flags=$2
    local awk_command='/elapsed time:/ { print $3 }'
    local iterations=$3

    make clean | tee -a $LOG_FILE
    MACRO_FLAGS=$macro_flags make | tee -a $LOG_FILE
    echo $1 >> $OUTPUT_FILE
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/cholesky/ocr" "cholesky" "--ds 100 --ts 10 --ps 1 --fi /home/berserker/main/src/ocr-1.1/xstg/apps/apps/cholesky/datasets/m_100.in"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/fft/ocr" "fft"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/examples/fib" "fib" "10"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/examples/quicksort" "quicksort"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/examples/smith-waterman" "smith-waterman" "10 10 /home/berserker/main/src/ocr-1.1/xstg/apps/apps/smithwaterman/datasets/string1-medium.txt /home/berserker/main/src/ocr-1.1/xstg/apps/apps/smithwaterman/datasets/string2-medium.txt /home/berserker/main/src/ocr-1.1/xstg/apps/apps/smithwaterman/datasets/score-medium.txt"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/examples/task-priorities" "task-priorities"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/nqueens/refactored/ocr" "nqueens" "13 5"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/uts-1.1/ocr" "uts"
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/RSBench/refactored/ocr/intel" "RSBench" "-d -s small -l 10000" 
    run_benchmark "$iterations" "$awk_command" "$APP_HOME/XSBench/refactored/ocr/intel" "XSBench" "-s small -g 100 -l 10000"
}

echo "" > $LOG_FILE
echo "" > $OUTPUT_FILE
run_benchmarks "jit" "-DMEASURE_TIME=1" 10
run_benchmarks "instrument" "-DMEASURE_TIME=1 -DINSTRUMENT=1" 10
run_benchmarks "race-detect" "-DMEASURE_TIME=1 -DINSTRUMENT=1 -DDETECT_RACE=1" 10

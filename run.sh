#!/bin/bash
#set -x

OCR_HOME=/home/berserker/main/workspace/Research/OCRDataRaceDetection/OCRDebugRuntime

APP_HOME=/home/berserker/main/src/ocr-1.1/xstg/apps/apps

export OCR_INSTALL=$OCR_HOME/install && export LD_LIBRARY_PATH=$OCR_INSTALL/lib:$APP_HOME/libs/install/x86/lib:$LD_LIBRARY_PATH && export OCR_CONFIG=race-detect.cfg && export CONFIG_NUM_THREADS=1

EXE_POS="$APP_HOME/examples/$1/install/x86/$1 ${@:2}"

RUN_TOOL='gdb --args'
echo $EXE_POS

#$EXE_POS
pin -t obj-intel64/OCR-VC.so -- $EXE_POS
#$RUN_TOOL pin -t obj-intel64/OCRViz.so -- $EXE_POS core
#pin -pause_tool 20 -t obj-intel64/OCRViz.so -- $EXE_POS
#/home/berserker/main/src/pin-3.2-81205-gcc-linux/pin -t obj-intel64/OCRViz.so -- $EXE_POS
#dot -Teps -o cg.eps cg.dot
#see cg.eps

#!/bin/bash
SCRIPT_DIR=$(dirname $(readlink -f ${BASH_SOURCE:-$0}))
cd $SCRIPT_DIR
source $SCRIPT_DIR/common/common.sh
commonSetup

./lhasa/lhasa.sh
exitOnError

./ffmpeg/ffmpeg.sh
exitOnError

./gimagereader/gimagereader.sh
exitOnError


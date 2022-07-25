#!/bin/bash
rm -rf release
mkdir -p release

cp -rf Synthimi *.{hpp,cpp,txt,json} LICENSE release/

mv release score-addon-synthimi
7z a score-addon-synthimi.zip score-addon-synthimi

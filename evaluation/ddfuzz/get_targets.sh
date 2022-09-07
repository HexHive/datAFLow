#!/bin/bash -e

git clone --no-checkout https://github.com/akimd/bison bison
git -C bison checkout 5555f4d

git clone --no-checkout https://github.com/PCRE2Project/pcre2 pcre2
git -C pcre2 checkout db53e40

git clone --no-checkout https://github.com/vnmakarov/mir mir
git -C mir checkout 852b1f2

git clone --no-checkout git://c9x.me/qbe.git qbe
git -C qbe checkout c8cd282

git clone --no-checkout https://github.com/grame-cncm/faust faust
git -C faust checkout 13def69

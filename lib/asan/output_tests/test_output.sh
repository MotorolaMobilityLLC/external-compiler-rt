#!/bin/bash

set -e # fail on any error

OS=`uname`
CXX=$1
CC=$2
FILE_CHECK=$3
CXXFLAGS="-mno-omit-leaf-frame-pointer -fno-omit-frame-pointer -fno-optimize-sibling-calls -g"
SYMBOLIZER=../scripts/asan_symbolize.py
ASAN_INTERFACE_H=../asan_interface.h
TMP_ASAN_REPORT=asan_report.tmp

run_program() {
  ./$1 2>&1 | $SYMBOLIZER 2> /dev/null | c++filt > $TMP_ASAN_REPORT
}

# check_program exe_file source_file check_prefixf
check_program() {
  run_program $1
  $FILE_CHECK $2 --check-prefix=$3 < $TMP_ASAN_REPORT
  rm -f $TMP_ASAN_REPORT
}

C_TEST=use-after-free
echo "Sanity checking a test in pure C"
$CC -g -faddress-sanitizer -O2 $C_TEST.c
check_program a.out $C_TEST.c CHECK
rm ./a.out

echo "Sanity checking a test in pure C with -pie"
$CC -g -faddress-sanitizer -O2 $C_TEST.c -pie
check_program a.out $C_TEST.c CHECK
rm ./a.out

echo "Testing sleep_before_dying"
$CC -g -faddress-sanitizer -O2 $C_TEST.c
export ASAN_OPTIONS="sleep_before_dying=1"
check_program a.out $C_TEST.c CHECKSLEEP
export ASAN_OPTIONS=""
rm ./a.out

echo "Checking strip_path_prefix option"
$CC -g -faddress-sanitizer -O2 $C_TEST.c
export ASAN_OPTIONS="strip_path_prefix='/'"
./a.out 2>&1 | $FILE_CHECK $C_TEST.c --check-prefix=CHECKSTRIP
export ASAN_OPTIONS=""
rm ./a.out

echo "Checking the presense of interface symbols in compiled file"
$CC -g -faddress-sanitizer -dead_strip -O2 $C_TEST.c
nm ./a.out | egrep " [TW] " | sed "s/.* T //" | sed "s/.* W //" | grep "__asan_" | sed "s/___asan_/__asan_/" > symbols.txt
cat $ASAN_INTERFACE_H | sed "s/\/\/.*//" | grep "__asan_.*("  | sed "s/.* __asan_/__asan_/;s/(.*//" > interface.txt
for i in __asan_report_{load,store}{1,2,4,8,16}
do
  echo $i >> interface.txt
done
cat interface.txt | sort | uniq | diff symbols.txt - || exit 1
rm ./a.out interface.txt symbols.txt


# FIXME: some tests do not need to be ran for all the combinations of arch
# and optimization mode.
for t in  *.cc; do
  for b in 32 64; do
    for O in 0 1 2 3; do
      c=`basename $t .cc`
      if [[ "$c" == *"-so" ]]; then
        continue
      fi
      if [[ "$c" == *"-linux" ]]; then
        if [[ "$OS" != "Linux" ]]; then
          continue
        fi
      fi
      c_so=$c-so
      exe=$c.$b.O$O
      so=$c.$b.O$O-so.so
      echo testing $exe
      build_command="$CXX $CXXFLAGS -m$b -faddress-sanitizer -O$O $c.cc -o $exe"
      [ "$DEBUG" == "1" ] && echo $build_command
      $build_command
      [ -e "$c_so.cc" ] && $CXX $CXXFLAGS -m$b -faddress-sanitizer -O$O $c_so.cc -fPIC -shared -o $so
      run_program $exe
      # Check common expected lines for OS.
      $FILE_CHECK $c.cc --check-prefix="Check-Common" < $TMP_ASAN_REPORT
      # Check OS-specific lines.
      if [ `grep -c "Check-$OS" $c.cc` -gt 0 ]
      then
        $FILE_CHECK $c.cc --check-prefix="Check-$OS" < $TMP_ASAN_REPORT
      fi
      rm ./$exe
      rm ./$TMP_ASAN_REPORT
      [ -e "$so" ] && rm ./$so
    done
  done
done

exit 0

env.CC="clang-3.9"
env.CXX="clang++-3.9"
env.CCC_CC="clang-3.9"
env.CCC_CXX="clang++-3.9"
env.SCAN_BUILD="scan-build-3.9"
env.PATH="$PATH:/usr/lib/llvm-3.9/bin/"
env.CMAKE_ARGS="-DCOUCHBASE_KV_COMMIT_VALIDATION=1 -DCMAKE_BUILD_TYPE=Debug -DEP_USE_ROCKSDB=ON"

// 2017-05-30: Ubuntu16.04 machines only have 4 CPUs - limit parallelism.
env.PARALLELISM=6

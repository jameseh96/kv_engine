env.AGENT_LABEL = "ubuntu-1604 && ${env.BRANCH_NAME}"

// 2018-02-01: cbdeps are now compiled with GCC 7 (see https://issues.couchbase.com/browse/CBD-2151).
// We therefore need to ensure that clang uses GCC7's libstdc++ (and not the default GCC 5). As GCC 7 is installed in /usr/local,
// clang doesn't automatically detect it, so we need to explicitly tell it to use the toolchain in /usr/local.
env.CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug -DCOUCHBASE_KV_COMMIT_VALIDATION=1 -DCMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN=/usr/local -DCMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN=/usr/local"

env.ENABLE_ADDRESSSANITIZER=1
env.ENABLE_UNDEFINEDSANITIZER=1
env.UBSAN_OPTIONS=print_stacktrace=1

env.PATH="$PATH:/usr/lib/llvm-3.9/bin/"

// 2017-05-26: ubuntu16.04 machines are currently 4 cores with 8GB RAM - constrain parallelism so we don't swap (too much)...
env.PARALLELISM=6
env.TEST_PARALLELISM=4

// Breakpad disabled as it deliberately crashes (which ASan doesn't like :)
// breakdancer disabled as it is very slow under ASan.
// MB-25989: Disabling rocksdb tests while they are not stable.
env.TESTS_EXCLUDE="memcached-breakpad-test-segfault|breakdancer|ep_testsuite_basic.value_eviction.rocksdb|ep_testsuite_basic.full_eviction.rocksdb|ep_testsuite.value_eviction.rocksdb|ep_testsuite.full_eviction.rocksdb|ep_testsuite_dcp.full_eviction.rocksdb|ep_testsuite_dcp.value_eviction.rocksdb|ep_testsuite_xdcr.full_eviction.rocksdb|ep_testsuite_xdcr.value_eviction.rocksdb|ep-engine-persistence-unit-tests.rocksdb"

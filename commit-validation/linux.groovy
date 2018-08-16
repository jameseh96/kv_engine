env.AGENT_LABEL = "ubuntu-1604 && ${env.BRANCH_NAME}"

env.CMAKE_ARGS="-DCOUCHBASE_KV_COMMIT_VALIDATION=1 -DPHOSPHOR_DISABLE=ON -DBUILD_ENTERPRISE=1"

// 2017-05-26: ubuntu16.04 machines are currently 4 cores with 8GB RAM - constrain parallelism so we don't swap (too much)...
env.PARALLELISM=6
env.TEST_PARALLELISM=3
env.TESTS_EXCLUDE="memcached-spdlogger-test"

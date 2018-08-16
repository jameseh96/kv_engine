env.AGENT_LABEL = "msvc2015"

env.CMAKE_ARGS="-DCOUCHBASE_KV_COMMIT_VALIDATION=1"
env.CMAKE_GENERATOR="Ninja"
env.TESTS_EXCLUDE="memcached-spdlogger-test|memcached_testapp.*.TransportProtocols/GetSetSnappyOnOffTest"

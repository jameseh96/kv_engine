def getJobName() {
    // e.g., kv_engine-ASan-UBSan/master
    //       [kv_engine-ASan-UBSan, master]
    //       [kv_engine, ASan-UBSan]
    //       [ASan-UBSan]
    return env.JOB_NAME.split("/")[0].split("-", 2)[1]
}

def getProjectName() {
    return env.GERRIT_PROJECT ? env.GERRIT_PROJECT : 'kv_engine'
}

def getManifestFileName() {
    if (env.BRANCH_NAME == 'master') {
        return 'branch-master.xml'
    } else {
        return "couchbase-server/${env.BRANCH_NAME}.xml"
    }
}

def addToEnv(closure) {
    closure.resolveStrategy = Closure.DELEGATE_FIRST
    closure.delegate = env
    closure()
}

def loadEnvFiles() {
    def base = "commit-validation/config/common/${getJobName()}.groovy"
    def overrides = "commit-validation/config/${getProjectName()}/${getJobName()}.groovy"
    for (fileName in [base, overrides]) {
        if (fileExists(fileName)) {
            def closure = load fileName
            addToEnv(closure)
        }
    }
}

properties([pipelineTriggers([gerrit(customUrl: '',
                                    serverName: 'review.couchbase.org',
                                    silentStartMode: true,
                                    gerritProjects: [[branches: [[compareType: 'PLAIN',
                                                                   pattern: env.BRANCH_NAME]],
                                                       compareType: 'PLAIN',
                                                       disableStrictForbiddenFileVerification: false,
                                                       pattern: getProjectName()]],
                                    triggerOnEvents: [patchsetCreated(excludeDrafts: true,
                                                                       excludeNoCodeChange: false,
                                                                       excludeTrivialRebase: false),
                                                      draftPublished()])])])
node {
    sh 'ls'
    sh 'pwd'
}

def call(name, nodeLabel) {
    pipeline {
        agent { label 'mac' }
        stages {
            stage("Checkout Repo"){
                steps {
                    checkout([$class: 'RepoScm',
                        currentBranch: true,
                        jobs: 8,
                        manifestFile: "${getManifestFileName()}",
                        manifestGroup: 'build,kv',
                        manifestRepositoryUrl: 'git://github.com/couchbase/manifest.git',
                        quiet: true,
                        resetFirst: false])  
                }
            }

            stage("Prep Environment"){
                steps {
                    loadEnvFiles()
                    
                    // Common script run by various Jenkins commit-validation builds.
                    //
                    // Checks out all the gerrit changes with change-ids matching
                    // $GERRIT_PATCHSET_REVISION in an appropriate branch for the current
                    // manifest from Gerrit server GERRIT_HOST:GERRIT_PORT, compiles and then
                    // runs unit tests for GERRIT_PROJECT (if applicable).
                    script { 
                        requiredEnvVars = ['GERRIT_HOST',
                                           'GERRIT_PORT',
                                           'GERRIT_PROJECT',
                                           'GERRIT_PATCHSET_REVISION',
                                           'GERRIT_REFSPEC',
                                           'GERRIT_CHANGE_ID']

                        
                        for (var in requiredEnvVars) {
                            if (!env.getProperty(var)){
                                echo "Error: Required environment variable '${var}' not set."
                            }
                        }

                        // Optional env vars - how many jobs to run in parallel by default?
                        env.PARALLELISM = env.PARALLELISM ? env.PARALLELISM : 8;

                        // Set default TEST_PARALLELISM to 4 - many of our tests are actually
                        // multi-threaded (unlike the compiler) and hence running 8 tests in
                        // parallel (each with multiple threads) can overload the CV machines
                        // and cause test timeouts.
                        env.TEST_PARALLELISM = env.TEST_PARALLELISM ? env.TEST_PARALLELISM : 4;

                        //env.BASEDIR = sh(returnStdout: true, script: 'cd $(dirname $BASH_SOURCE) && pwd').trim()
                        env.BASEDIR = sh(returnStdout: true, script: 'pwd').trim() + '/cbbuild/scripts/jenkins/commit_validation/'

                        // source ~jenkins/.bash_profile

                        // CCACHE is good - use it if available.
                        env.PATH = "/usr/lib/ccache:${env.PATH}"


                        if (env.ENABLE_CODE_COVERAGE) {
                           env.CMAKE_ARGS="${CMAKE_ARGS} -DCB_CODE_COVERAGE=ON"
                        }
                        if (env.ENABLE_THREADSANITIZER) {
                           env.CMAKE_ARGS="${CMAKE_ARGS} -DCB_THREADSANITIZER=ON"
                        }
                        if (env.ENABLE_ADDRESSSANITIZER) {
                           env.CMAKE_ARGS="${CMAKE_ARGS} -DCB_ADDRESSSANITIZER=${ENABLE_ADDRESSSANITIZER}"
                        }
                        if (env.ENABLE_UNDEFINEDSANITIZER) {
                           env.CMAKE_ARGS="${CMAKE_ARGS} -DCB_UNDEFINEDSANITIZER=1"
                        }
                        if (env.ENABLE_CBDEPS_TESTING) {
                           env.CMAKE_ARGS="${CMAKE_ARGS} -DCB_DOWNLOAD_DEPS_REPO=http://latestbuilds.service.couchbase.com/builds/releases/cbdeps"
                        }
                    }
                    sh 'ulimit -a'
                    echo ""
                    sh 'env | grep -iv password | grep -iv passwd | sort'
                }
            }

            stage("Clean"){
                steps {
                    sh 'make clean-xfd-hard'
                    sh 'rm -fr install'
                    sh 'rm -f build/CMakeCache.txt'

                    // Zero ccache stats, so we can measure how much space this build is
                    // consuming.
                    sh 'ccache -z || true'

                    // Wipe out any core files left from a previous run.
                    sh ' rm -f /tmp/core.*'
                }
            }

            stage("Build"){
                steps {
                    //sh '${BASEDIR}/checkout_dependencies.py $GERRIT_PATCHSET_REVISION $GERRIT_CHANGE_ID $GERRIT_PROJECT $GERRIT_REFSPEC'
                    script {
                        if (env.ENABLE_CBDEPS_TESTING) {
                           sh 'rm -rf ~/.cbdepscache'
                           sh 'rm -rf build/tlm/deps'
                        }
                    }
                    
                    sh 'make -j${PARALLELISM} EXTRA_CMAKE_OPTIONS="${CMAKE_ARGS}"'

                    sh 'ccache -s || true'
                    script {
                        if (env.GERRIT_PROJECT == "ns_server") {
                            env.BUILD_DIR="${GERRIT_PROJECT}/build"
                        }
                        else if (env.GOPROJECT) {
                            env.BUILD_DIR="build/goproj/src/github.com/couchbase/${GERRIT_PROJECT}"
                        } else {
                            env.BUILD_DIR="build/${GERRIT_PROJECT}"
                        }
                    }

                    echo env.CMAKE_ARGS
                }
            }

            stage("Test"){
                steps {
                    dir(BUILD_DIR) {
                        script {
                            if (env.ENABLE_CODE_COVERAGE) {
                                // Reset code coverage counters (note optional hence the || true).
                                sh 'make ${GERRIT_PROJECT}-coverage-zero-counters || true'
                            }

                            // -j${TEST_PARALLELISM} : Run tests in parallel.
                            // -T Test   : Generate XML output file of test results.
                            sh 'make test ARGS="-j${TEST_PARALLELISM} --output-on-failure --no-compress-output -T Test --exclude-regex ${TESTS_EXCLUDE}"'

                            // Generate code coverage report in XML format for Jenkins plugin.
                            if (env.ENABLE_CODE_COVERAGE) {
                                sh 'make ${GERRIT_PROJECT}-coverage-report-xml || true'
                            }

                            if (env.RUN_TESTS_UNDER_VALGRIND) {
                                // Clear out any previous runs' output files
                                sh 'find . -name "memcheck.*.xml" -delete'
                                sh 'make test ARGS="-j${TEST_PARALLELISM} --output-on-failure --no-compress-output -D ExperimentalMemCheck --exclude-regex ${VALGRIND_TESTS_EXCLUDE}"'
                                // As part our test execution we run system commands which
                                // unfortunately have leaks themselves
                                // (e.g. /bin/sh). Therefore remove any results from such
                                // programs Jenkins parses the results so we don't include
                                // them.
                                def newfiles = sh(returnStdout: true, script: 'find . -name "memcheck.*.xml"')
                                sh "${BASEDIR}/remove_irrelevant_memcheck_results.py ${newfiles}"
                            }
                        }
                    }
       
                    echo "TEST"
                    xunit testTimeMargin: '3000', thresholdMode: 1, thresholds: [failed(failureThreshold: '0'), skipped(failureThreshold: '0')], tools: [CTest(deleteOutputFiles: true, failIfNotNew: true, pattern: "build/${getProjectName()}/Testing/**/Test.xml", skipNoTestFiles: false, stopProcessingIfError: true)]
                }
            }
        }
        post {
            always {
                echo "ALWAYS"
            }
            failure {
                echo "FAILED"
                //mail to: team@example.com, subject: 'The Pipeline failed :('
            }
        }
    }
}

def closureToMap(closure, initialValues=[:]) {
    def map = initialValues
    closure.resolveStrategy = Closure.DELEGATE_ONLY
    closure.delegate = map
    closure()
    return map
}

def getManifestFileName() {
    if (env.BRANCH_NAME == 'master') {
        return 'branch-master.xml'
    } else {
        return "couchbase-server/${env.BRANCH_NAME}.xml"
    }
}

def withConfEnv(env, envOverrides, body) {
    def envMap = closureToMap(envOverrides, ["env":env])
    withEnv (envMap.collect { key, value -> key + "=" + value }, body)
}

properties([pipelineTriggers([gerrit(customUrl: '',
                                    serverName: 'review.couchbase.org',
                                    silentStartMode: true,
                                    gerritProjects: [[branches: [[compareType: 'PLAIN',
                                                                   pattern: env.BRANCH_NAME]],
                                                       compareType: 'PLAIN',
                                                       disableStrictForbiddenFileVerification: false,
                                                       pattern: 'kv_engine']],
                                    triggerOnEvents: [patchsetCreated(excludeDrafts: true,
                                                                       excludeNoCodeChange: false,
                                                                       excludeTrivialRebase: false),
                                                      draftPublished()])])])

def call(env, body) {
    // evaluate the body block, and collect configuration into the object
    def params = closureToMap(body)

    pipeline {
        agent none
        stages {
            stage("Run single-project"){
                agent { label 'mac' }
                steps {
                    checkout([$class: 'RepoScm',
                        currentBranch: true,
                        jobs: 8,
                        manifestFile: "${getManifestFileName()}",
                        manifestGroup: 'build,kv',
                        manifestRepositoryUrl: 'git://github.com/couchbase/manifest.git',
                        quiet: true,
                        resetFirst: false]) 
                    echo params.label
                    echo env.PATH
                    //echo params.env
                    sh 'env'
                    sh 'which env'
                    withConfEnv(env, params.envConf) {
                        sh '/usr/bin/env'
                    }
                }
            }
        }
    }
}


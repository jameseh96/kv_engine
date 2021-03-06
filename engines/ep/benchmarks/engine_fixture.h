/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include <benchmark/benchmark.h>

#include "item.h"

class BenchmarkMemoryTracker;
class SingleThreadedExecutorPool;
class SynchronousEPEngine;

/**
 * A fixture for benchmarking EpEngine and related classes.
 */
class EngineFixture : public benchmark::Fixture {
protected:
    void SetUp(const benchmark::State& state) override;

    void TearDown(const benchmark::State& state) override;

    Item make_item(uint16_t vbid,
                   const std::string& key,
                   const std::string& value);

    std::unique_ptr<SynchronousEPEngine> engine;
    const void* cookie = nullptr;
    const int vbid = 0;

    // Allows subclasses to add stuff to the config
    std::string varConfig;
    SingleThreadedExecutorPool* executorPool;
};

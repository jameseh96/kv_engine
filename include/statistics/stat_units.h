/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include <array>
#include <ratio>
#include <string>

namespace cb::stats {

// Enum of the relevant base units used by convention in Prometheus
enum class BaseUnit {
    None, // used for text stats where units aren't relevant
    Count, // Generic count which does not meet a better unit
    Seconds,
    Bytes,
    Percent
};

class Unit {
public:
    constexpr Unit(BaseUnit baseUnit) : Unit(std::ratio<1>{}, baseUnit) {
    }

    template <class RatioType>
    constexpr Unit(RatioType ratio, BaseUnit baseUnit)
        : numerator(RatioType::num),
          denominator(RatioType::den),
          baseUnit(baseUnit) {
    }

    /**
     * Scale a value of the current unit (e.g., milliseconds) to the base
     * unit (e.g., seconds).
     */
    double normalise(double value) {
        return (value * numerator) / denominator;
    }

    std::string getUnitSuffix() {
        switch (baseUnit) {
        case BaseUnit::None:
            return "";
        case BaseUnit::Count:
            return "";
        case BaseUnit::Seconds:
            return "_seconds";
        case BaseUnit::Bytes:
            return "_bytes";
        case BaseUnit::Percent:
            return "_percent";
        }
    }

private:
    std::intmax_t numerator;
    std::intmax_t denominator;
    BaseUnit baseUnit;
};

namespace units {
constexpr Unit none{std::ratio<1>{}, BaseUnit::None};

constexpr Unit count{std::ratio<1>{}, BaseUnit::Count};

constexpr Unit percent{std::ratio<1, 100>{}, BaseUnit::Percent};
// floating point between 0 and 1
constexpr Unit ratio{std::ratio<1>{}, BaseUnit::Percent};

// time units
constexpr Unit picoseconds{std::pico{}, BaseUnit::Seconds};
constexpr Unit nanoseconds{std::nano{}, BaseUnit::Seconds};
constexpr Unit microseconds{std::micro{}, BaseUnit::Seconds};
constexpr Unit milliseconds{std::milli{}, BaseUnit::Seconds};
constexpr Unit seconds{std::ratio<1>{}, BaseUnit::Seconds};
constexpr Unit minutes{std::ratio<60>{}, BaseUnit::Seconds};
constexpr Unit hours{std::ratio<60 * 60>{}, BaseUnit::Seconds};
constexpr Unit days{std::ratio<60 * 60 * 24>{}, BaseUnit::Seconds};

// byte units
constexpr Unit bits{std::ratio<1, 8>{}, BaseUnit::Bytes};
constexpr Unit bytes{std::ratio<1>{}, BaseUnit::Bytes};
constexpr Unit kilobytes{std::kilo{}, BaseUnit::Bytes};
constexpr Unit megabytes{std::mega{}, BaseUnit::Bytes};
constexpr Unit gigabytes{std::giga{}, BaseUnit::Bytes};
constexpr Unit terabytes{std::tera{}, BaseUnit::Bytes};
} // namespace units
} // namespace cb::stats
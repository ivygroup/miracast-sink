/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "TimeSeries.h"

#include <math.h>
#include <string.h>

namespace android {

TimeSeries::TimeSeries()
    : mCount(0),
      mSum(0.0) {
}

void TimeSeries::add(double val) {
    if (mCount < kHistorySize) {
        mValues[mCount++] = val;
        mSum += val;
    } else {
        mSum -= mValues[0];
        memmove(&mValues[0], &mValues[1], (kHistorySize - 1) * sizeof(double));
        mValues[kHistorySize - 1] = val;
        mSum += val;
    }
}

double TimeSeries::mean() const {
    if (mCount < 1) {
        return 0.0;
    }

    return mSum / mCount;
}

double TimeSeries::sdev() const {
    if (mCount < 1) {
        return 0.0;
    }

    double m = mean();

    double sum = 0.0;
    for (size_t i = 0; i < mCount; ++i) {
        double tmp = mValues[i] - m;
        tmp *= tmp;

        sum += tmp;
    }

    return sqrt(sum / mCount);
}

}  // namespace android

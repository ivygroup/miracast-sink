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

#ifndef LINEAR_REGRESSION_H_

#define LINEAR_REGRESSION_H_

#include <sys/types.h>
#include <media/stagefright/foundation/ABase.h>

namespace android {

// Helper class to fit a line to a set of points minimizing the sum of
// squared (orthogonal) distances from line to individual points.
struct LinearRegression {
    LinearRegression(size_t historySize);
    ~LinearRegression();

    void addPoint(float x, float y);

    bool approxLine(float *n1, float *n2, float *b) const;

private:
    struct Point {
        float mX, mY;
    };

    size_t mHistorySize;
    size_t mCount;
    Point *mHistory;

    float mSumX, mSumY;

    DISALLOW_EVIL_CONSTRUCTORS(LinearRegression);
};

}  // namespace android

#endif  // LINEAR_REGRESSION_H_

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

#ifndef CONVERTER_H_

#define CONVERTER_H_

#include "WifiDisplaySource.h"

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct MediaCodec;

#define ENABLE_SILENCE_DETECTION        0

// Utility class that receives media access units and converts them into
// media access unit of a different format.
// Right now this'll convert raw video into H.264 and raw audio into AAC.
struct Converter : public AHandler {
    Converter(
            const sp<AMessage> &notify,
            const sp<ALooper> &codecLooper,
            const sp<AMessage> &format,
            bool usePCMAudio);

    status_t initCheck() const;

    size_t getInputBufferCount() const;

    sp<AMessage> getOutputFormat() const;
    bool needToManuallyPrependSPSPPS() const;

    void feedAccessUnit(const sp<ABuffer> &accessUnit);
    void signalEOS();

    void requestIDRFrame();

    enum {
        kWhatAccessUnit,
        kWhatEOS,
        kWhatError,
    };

    enum {
        kWhatDoMoreWork,
        kWhatRequestIDRFrame,
        kWhatShutdown,
        kWhatMediaPullerNotify,
        kWhatEncoderActivity,
    };

    void shutdownAsync();

protected:
    virtual ~Converter();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    status_t mInitCheck;
    sp<AMessage> mNotify;
    sp<ALooper> mCodecLooper;
    sp<AMessage> mInputFormat;
    bool mIsVideo;
    bool mIsPCMAudio;
    sp<AMessage> mOutputFormat;
    bool mNeedToManuallyPrependSPSPPS;

    sp<MediaCodec> mEncoder;
    sp<AMessage> mEncoderActivityNotify;

    Vector<sp<ABuffer> > mEncoderInputBuffers;
    Vector<sp<ABuffer> > mEncoderOutputBuffers;

    List<size_t> mAvailEncoderInputIndices;

    List<sp<ABuffer> > mInputBufferQueue;

    bool mDoMoreWorkPending;

#if ENABLE_SILENCE_DETECTION
    int64_t mFirstSilentFrameUs;
    bool mInSilentMode;
#endif

    sp<ABuffer> mPartialAudioAU;

    status_t initEncoder();

    status_t feedEncoderInputBuffers();

    void scheduleDoMoreWork();
    status_t doMoreWork();

    void notifyError(status_t err);

    // Packetizes raw PCM audio data available in mInputBufferQueue
    // into a format suitable for transport stream inclusion and
    // notifies the observer.
    status_t feedRawAudioInputBuffers();

    static bool IsSilence(const sp<ABuffer> &accessUnit);

    DISALLOW_EVIL_CONSTRUCTORS(Converter);
};

}  // namespace android

#endif  // CONVERTER_H_

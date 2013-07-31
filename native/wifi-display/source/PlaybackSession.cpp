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

//#define LOG_NDEBUG 0
#define LOG_TAG "PlaybackSession"
#include <utils/Log.h>

#include "PlaybackSession.h"

#include "Converter.h"
#include "MediaPuller.h"
#include "RepeaterSource.h"
#include "Sender.h"
#include "TSPacketizer.h"
#include "include/avc_utils.h"
#include "WifiDisplaySource.h"

#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <media/IHDCP.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MPEG2TSWriter.h>
#include <media/stagefright/SurfaceMediaSource.h>
#include <media/stagefright/Utils.h>

#include <OMX_IVCommon.h>

namespace android {

struct WifiDisplaySource::PlaybackSession::Track : public AHandler {
    enum {
        kWhatStopped,
    };

    Track(const sp<AMessage> &notify,
          const sp<ALooper> &pullLooper,
          const sp<ALooper> &codecLooper,
          const sp<MediaPuller> &mediaPuller,
          const sp<Converter> &converter);

    void setRepeaterSource(const sp<RepeaterSource> &source);

    sp<AMessage> getFormat();
    bool isAudio() const;

    const sp<Converter> &converter() const;
    ssize_t packetizerTrackIndex() const;

    void setPacketizerTrackIndex(size_t index);

    status_t start();
    void stopAsync();

    void queueAccessUnit(const sp<ABuffer> &accessUnit);
    sp<ABuffer> dequeueAccessUnit();

    bool hasOutputBuffer(int64_t *timeUs) const;
    void queueOutputBuffer(const sp<ABuffer> &accessUnit);
    sp<ABuffer> dequeueOutputBuffer();

#if SUSPEND_VIDEO_IF_IDLE
    bool isSuspended() const;
#endif

    size_t countQueuedOutputBuffers() const {
        return mQueuedOutputBuffers.size();
    }

    void requestIDRFrame();

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~Track();

private:
    enum {
        kWhatMediaPullerStopped,
    };

    sp<AMessage> mNotify;
    sp<ALooper> mPullLooper;
    sp<ALooper> mCodecLooper;
    sp<MediaPuller> mMediaPuller;
    sp<Converter> mConverter;
    bool mStarted;
    ssize_t mPacketizerTrackIndex;
    bool mIsAudio;
    List<sp<ABuffer> > mQueuedAccessUnits;
    sp<RepeaterSource> mRepeaterSource;
    List<sp<ABuffer> > mQueuedOutputBuffers;
    int64_t mLastOutputBufferQueuedTimeUs;

    static bool IsAudioFormat(const sp<AMessage> &format);

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

WifiDisplaySource::PlaybackSession::Track::Track(
        const sp<AMessage> &notify,
        const sp<ALooper> &pullLooper,
        const sp<ALooper> &codecLooper,
        const sp<MediaPuller> &mediaPuller,
        const sp<Converter> &converter)
    : mNotify(notify),
      mPullLooper(pullLooper),
      mCodecLooper(codecLooper),
      mMediaPuller(mediaPuller),
      mConverter(converter),
      mStarted(false),
      mPacketizerTrackIndex(-1),
      mIsAudio(IsAudioFormat(mConverter->getOutputFormat())),
      mLastOutputBufferQueuedTimeUs(-1ll) {
}

WifiDisplaySource::PlaybackSession::Track::~Track() {
    CHECK(!mStarted);
}

// static
bool WifiDisplaySource::PlaybackSession::Track::IsAudioFormat(
        const sp<AMessage> &format) {
    AString mime;
    CHECK(format->findString("mime", &mime));

    return !strncasecmp(mime.c_str(), "audio/", 6);
}

sp<AMessage> WifiDisplaySource::PlaybackSession::Track::getFormat() {
    return mConverter->getOutputFormat();
}

bool WifiDisplaySource::PlaybackSession::Track::isAudio() const {
    return mIsAudio;
}

const sp<Converter> &WifiDisplaySource::PlaybackSession::Track::converter() const {
    return mConverter;
}

ssize_t WifiDisplaySource::PlaybackSession::Track::packetizerTrackIndex() const {
    return mPacketizerTrackIndex;
}

void WifiDisplaySource::PlaybackSession::Track::setPacketizerTrackIndex(size_t index) {
    CHECK_LT(mPacketizerTrackIndex, 0);
    mPacketizerTrackIndex = index;
}

status_t WifiDisplaySource::PlaybackSession::Track::start() {
    ALOGV("Track::start isAudio=%d", mIsAudio);

    CHECK(!mStarted);

    status_t err = OK;

    if (mMediaPuller != NULL) {
        err = mMediaPuller->start();
    }

    if (err == OK) {
        mStarted = true;
    }

    return err;
}

void WifiDisplaySource::PlaybackSession::Track::stopAsync() {
    ALOGV("Track::stopAsync isAudio=%d", mIsAudio);

    mConverter->shutdownAsync();

    sp<AMessage> msg = new AMessage(kWhatMediaPullerStopped, id());

    if (mStarted && mMediaPuller != NULL) {
        if (mRepeaterSource != NULL) {
            // Let's unblock MediaPuller's MediaSource::read().
            mRepeaterSource->wakeUp();
        }

        mMediaPuller->stopAsync(msg);
    } else {
        msg->post();
    }
}

void WifiDisplaySource::PlaybackSession::Track::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatMediaPullerStopped:
        {
            mConverter.clear();

            mStarted = false;

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStopped);
            notify->post();

            ALOGI("kWhatStopped %s posted", mIsAudio ? "audio" : "video");
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySource::PlaybackSession::Track::queueAccessUnit(
        const sp<ABuffer> &accessUnit) {
    mQueuedAccessUnits.push_back(accessUnit);
}

sp<ABuffer> WifiDisplaySource::PlaybackSession::Track::dequeueAccessUnit() {
    if (mQueuedAccessUnits.empty()) {
        return NULL;
    }

    sp<ABuffer> accessUnit = *mQueuedAccessUnits.begin();
    CHECK(accessUnit != NULL);

    mQueuedAccessUnits.erase(mQueuedAccessUnits.begin());

    return accessUnit;
}

void WifiDisplaySource::PlaybackSession::Track::setRepeaterSource(
        const sp<RepeaterSource> &source) {
    mRepeaterSource = source;
}

void WifiDisplaySource::PlaybackSession::Track::requestIDRFrame() {
    if (mIsAudio) {
        return;
    }

    if (mRepeaterSource != NULL) {
        mRepeaterSource->wakeUp();
    }

    mConverter->requestIDRFrame();
}

bool WifiDisplaySource::PlaybackSession::Track::hasOutputBuffer(
        int64_t *timeUs) const {
    *timeUs = 0ll;

    if (mQueuedOutputBuffers.empty()) {
        return false;
    }

    const sp<ABuffer> &outputBuffer = *mQueuedOutputBuffers.begin();

    CHECK(outputBuffer->meta()->findInt64("timeUs", timeUs));

    return true;
}

void WifiDisplaySource::PlaybackSession::Track::queueOutputBuffer(
        const sp<ABuffer> &accessUnit) {
    mQueuedOutputBuffers.push_back(accessUnit);
    mLastOutputBufferQueuedTimeUs = ALooper::GetNowUs();
}

sp<ABuffer> WifiDisplaySource::PlaybackSession::Track::dequeueOutputBuffer() {
    CHECK(!mQueuedOutputBuffers.empty());

    sp<ABuffer> outputBuffer = *mQueuedOutputBuffers.begin();
    mQueuedOutputBuffers.erase(mQueuedOutputBuffers.begin());

    return outputBuffer;
}

#if SUSPEND_VIDEO_IF_IDLE
bool WifiDisplaySource::PlaybackSession::Track::isSuspended() const {
    if (!mQueuedOutputBuffers.empty()) {
        return false;
    }

    if (mLastOutputBufferQueuedTimeUs < 0ll) {
        // We've never seen an output buffer queued, but tracks start
        // out live, not suspended.
        return false;
    }

    // If we've not seen new output data for 60ms or more, we consider
    // this track suspended for the time being.
    return (ALooper::GetNowUs() - mLastOutputBufferQueuedTimeUs) > 60000ll;
}
#endif

////////////////////////////////////////////////////////////////////////////////

WifiDisplaySource::PlaybackSession::PlaybackSession(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify,
        const in_addr &interfaceAddr,
        const sp<IHDCP> &hdcp)
    : mNetSession(netSession),
      mNotify(notify),
      mInterfaceAddr(interfaceAddr),
      mHDCP(hdcp),
      mWeAreDead(false),
      mLastLifesignUs(),
      mVideoTrackIndex(-1),
      mPrevTimeUs(-1ll),
      mAllTracksHavePacketizerIndex(false) {
}

status_t WifiDisplaySource::PlaybackSession::init(
        const char *clientIP, int32_t clientRtp, int32_t clientRtcp,
        Sender::TransportMode transportMode,
        bool usePCMAudio) {
    status_t err = setupPacketizer(usePCMAudio);

    if (err != OK) {
        return err;
    }

    sp<AMessage> notify = new AMessage(kWhatSenderNotify, id());
    mSender = new Sender(mNetSession, notify);

    mSenderLooper = new ALooper;
    mSenderLooper->setName("sender_looper");

    mSenderLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    mSenderLooper->registerHandler(mSender);

    err = mSender->init(clientIP, clientRtp, clientRtcp, transportMode);

    if (err != OK) {
        return err;
    }

    updateLiveness();

    return OK;
}

WifiDisplaySource::PlaybackSession::~PlaybackSession() {
}

int32_t WifiDisplaySource::PlaybackSession::getRTPPort() const {
    return mSender->getRTPPort();
}

int64_t WifiDisplaySource::PlaybackSession::getLastLifesignUs() const {
    return mLastLifesignUs;
}

void WifiDisplaySource::PlaybackSession::updateLiveness() {
    mLastLifesignUs = ALooper::GetNowUs();
}

status_t WifiDisplaySource::PlaybackSession::play() {
    updateLiveness();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::finishPlay() {
    // XXX Give the dongle a second to bind its sockets.
    (new AMessage(kWhatFinishPlay, id()))->post(1000000ll);
    return OK;
}

status_t WifiDisplaySource::PlaybackSession::onFinishPlay() {
    return mSender->finishInit();
}

status_t WifiDisplaySource::PlaybackSession::onFinishPlay2() {
    mSender->scheduleSendSR();

    for (size_t i = 0; i < mTracks.size(); ++i) {
        CHECK_EQ((status_t)OK, mTracks.editValueAt(i)->start());
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionEstablished);
    notify->post();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::pause() {
    updateLiveness();

    return OK;
}

void WifiDisplaySource::PlaybackSession::destroyAsync() {
    ALOGI("destroyAsync");

    for (size_t i = 0; i < mTracks.size(); ++i) {
        mTracks.valueAt(i)->stopAsync();
    }
}

void WifiDisplaySource::PlaybackSession::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatConverterNotify:
        {
            if (mWeAreDead) {
                ALOGV("dropping msg '%s' because we're dead",
                      msg->debugString().c_str());

                break;
            }

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Converter::kWhatAccessUnit) {
                const sp<Track> &track = mTracks.valueFor(trackIndex);

                ssize_t packetizerTrackIndex = track->packetizerTrackIndex();

                if (packetizerTrackIndex < 0) {
                    sp<AMessage> trackFormat = track->getFormat()->dup();
                    if (mHDCP != NULL && !track->isAudio()) {
                        // HDCP2.0 _and_ HDCP 2.1 specs say to set the version
                        // inside the HDCP descriptor to 0x20!!!
                        trackFormat->setInt32("hdcp-version", 0x20);
                    }
                    packetizerTrackIndex = mPacketizer->addTrack(trackFormat);

                    CHECK_GE(packetizerTrackIndex, 0);

                    track->setPacketizerTrackIndex(packetizerTrackIndex);

                    if (allTracksHavePacketizerIndex()) {
                        status_t err = packetizeQueuedAccessUnits();

                        if (err != OK) {
                            notifySessionDead();
                            break;
                        }
                    }
                }

                sp<ABuffer> accessUnit;
                CHECK(msg->findBuffer("accessUnit", &accessUnit));

                if (!allTracksHavePacketizerIndex()) {
                    track->queueAccessUnit(accessUnit);
                    break;
                }

                track->queueOutputBuffer(accessUnit);

                drainAccessUnits();
                break;
            } else if (what == Converter::kWhatEOS) {
                CHECK_EQ(what, (status_t)Converter::kWhatEOS);

                ALOGI("output EOS on track %d", trackIndex);

                ssize_t index = mTracks.indexOfKey(trackIndex);
                CHECK_GE(index, 0);

                const sp<Converter> &converter =
                    mTracks.valueAt(index)->converter();
                looper()->unregisterHandler(converter->id());

                mTracks.removeItemsAt(index);

                if (mTracks.isEmpty()) {
                    ALOGI("Reached EOS");
                }
            } else {
                CHECK_EQ(what, (status_t)Converter::kWhatError);

                status_t err;
                CHECK(msg->findInt32("err", &err));

                ALOGE("converter signaled error %d", err);

                notifySessionDead();
            }
            break;
        }

        case kWhatSenderNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == Sender::kWhatInitDone) {
                onFinishPlay2();
            } else if (what == Sender::kWhatSessionDead) {
                notifySessionDead();
            } else {
                TRESPASS();
            }

            break;
        }

        case kWhatFinishPlay:
        {
            onFinishPlay();
            break;
        }

        case kWhatTrackNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Track::kWhatStopped) {
                ALOGI("Track %d stopped", trackIndex);

                sp<Track> track = mTracks.valueFor(trackIndex);
                looper()->unregisterHandler(track->id());
                mTracks.removeItem(trackIndex);
                track.clear();

                if (!mTracks.isEmpty()) {
                    ALOGI("not all tracks are stopped yet");
                    break;
                }

                mSenderLooper->unregisterHandler(mSender->id());
                mSender.clear();
                mSenderLooper.clear();

                mPacketizer.clear();

                sp<AMessage> notify = mNotify->dup();
                notify->setInt32("what", kWhatSessionDestroyed);
                notify->post();
            }
            break;
        }

        case kWhatPacketize:
        {
            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            sp<ABuffer> accessUnit;
            CHECK(msg->findBuffer("accessUnit", &accessUnit));

#if 0
            if ((ssize_t)trackIndex == mVideoTrackIndex) {
                int64_t nowUs = ALooper::GetNowUs();
                static int64_t prevNowUs = 0ll;

                ALOGI("sending AU, dNowUs=%lld us", nowUs - prevNowUs);

                prevNowUs = nowUs;
            }
#endif

            break;
        }

        default:
            TRESPASS();
    }
}

status_t WifiDisplaySource::PlaybackSession::setupPacketizer(bool usePCMAudio) {
    mPacketizer = new TSPacketizer;

    status_t err = addVideoSource();

    if (err != OK) {
        return err;
    }

    return addAudioSource(usePCMAudio);
}

status_t WifiDisplaySource::PlaybackSession::addSource(
        bool isVideo, const sp<MediaSource> &source, bool isRepeaterSource,
        bool usePCMAudio, size_t *numInputBuffers) {
    CHECK(!usePCMAudio || !isVideo);
    CHECK(!isRepeaterSource || isVideo);

    sp<ALooper> pullLooper = new ALooper;
    pullLooper->setName("pull_looper");

    pullLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    sp<ALooper> codecLooper = new ALooper;
    codecLooper->setName("codec_looper");

    codecLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    size_t trackIndex;

    sp<AMessage> notify;

    trackIndex = mTracks.size();

    sp<AMessage> format;
    status_t err = convertMetaDataToMessage(source->getFormat(), &format);
    CHECK_EQ(err, (status_t)OK);

    if (isVideo) {
        format->setInt32("store-metadata-in-buffers", true);

        format->setInt32(
                "color-format", OMX_COLOR_FormatAndroidOpaque);
    }

    notify = new AMessage(kWhatConverterNotify, id());
    notify->setSize("trackIndex", trackIndex);

    sp<Converter> converter =
        new Converter(notify, codecLooper, format, usePCMAudio);

    err = converter->initCheck();
    if (err != OK) {
        ALOGE("%s converter returned err %d", isVideo ? "video" : "audio", err);
        return err;
    }

    looper()->registerHandler(converter);

    notify = new AMessage(Converter::kWhatMediaPullerNotify, converter->id());
    notify->setSize("trackIndex", trackIndex);

    sp<MediaPuller> puller = new MediaPuller(source, notify);
    pullLooper->registerHandler(puller);

    if (numInputBuffers != NULL) {
        *numInputBuffers = converter->getInputBufferCount();
    }

    notify = new AMessage(kWhatTrackNotify, id());
    notify->setSize("trackIndex", trackIndex);

    sp<Track> track = new Track(
            notify, pullLooper, codecLooper, puller, converter);

    if (isRepeaterSource) {
        track->setRepeaterSource(static_cast<RepeaterSource *>(source.get()));
    }

    looper()->registerHandler(track);

    mTracks.add(trackIndex, track);

    if (isVideo) {
        mVideoTrackIndex = trackIndex;
    }

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addVideoSource() {
    sp<SurfaceMediaSource> source = new SurfaceMediaSource(width(), height());

    source->setUseAbsoluteTimestamps();

#if 1
    sp<RepeaterSource> videoSource =
        new RepeaterSource(source, 30.0 /* rateHz */);
#endif

#if 1
    size_t numInputBuffers;
    status_t err = addSource(
            true /* isVideo */, videoSource, true /* isRepeaterSource */,
            false /* usePCMAudio */, &numInputBuffers);
#else
    size_t numInputBuffers;
    status_t err = addSource(
            true /* isVideo */, source, false /* isRepeaterSource */,
            false /* usePCMAudio */, &numInputBuffers);
#endif

    if (err != OK) {
        return err;
    }

    err = source->setMaxAcquiredBufferCount(numInputBuffers);
    CHECK_EQ(err, (status_t)OK);

    mBufferQueue = source->getBufferQueue();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addAudioSource(bool usePCMAudio) {
    sp<AudioSource> audioSource = new AudioSource(
            AUDIO_SOURCE_REMOTE_SUBMIX,
            48000 /* sampleRate */,
            2 /* channelCount */);

    if (audioSource->initCheck() == OK) {
        return addSource(
                false /* isVideo */, audioSource, false /* isRepeaterSource */,
                usePCMAudio, NULL /* numInputBuffers */);
    }

    ALOGW("Unable to instantiate audio source");

    return OK;
}

sp<ISurfaceTexture> WifiDisplaySource::PlaybackSession::getSurfaceTexture() {
    return mBufferQueue;
}

int32_t WifiDisplaySource::PlaybackSession::width() const {
#if USE_1080P
    return 1920;
#else
    return 1280;
#endif
}

int32_t WifiDisplaySource::PlaybackSession::height() const {
#if USE_1080P
    return 1080;
#else
    return 720;
#endif
}

void WifiDisplaySource::PlaybackSession::requestIDRFrame() {
    for (size_t i = 0; i < mTracks.size(); ++i) {
        const sp<Track> &track = mTracks.valueAt(i);

        track->requestIDRFrame();
    }
}

bool WifiDisplaySource::PlaybackSession::allTracksHavePacketizerIndex() {
    if (mAllTracksHavePacketizerIndex) {
        return true;
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        if (mTracks.valueAt(i)->packetizerTrackIndex() < 0) {
            return false;
        }
    }

    mAllTracksHavePacketizerIndex = true;

    return true;
}

status_t WifiDisplaySource::PlaybackSession::packetizeAccessUnit(
        size_t trackIndex, sp<ABuffer> accessUnit,
        sp<ABuffer> *packets) {
    const sp<Track> &track = mTracks.valueFor(trackIndex);

    uint32_t flags = 0;

    bool isHDCPEncrypted = false;
    uint64_t inputCTR;
    uint8_t HDCP_private_data[16];

    bool manuallyPrependSPSPPS =
        !track->isAudio()
        && track->converter()->needToManuallyPrependSPSPPS()
        && IsIDR(accessUnit);

    if (mHDCP != NULL && !track->isAudio()) {
        isHDCPEncrypted = true;

        if (manuallyPrependSPSPPS) {
            accessUnit = mPacketizer->prependCSD(
                    track->packetizerTrackIndex(), accessUnit);
        }

        status_t err = mHDCP->encrypt(
                accessUnit->data(), accessUnit->size(),
                trackIndex  /* streamCTR */,
                &inputCTR,
                accessUnit->data());

        if (err != OK) {
            ALOGE("Failed to HDCP-encrypt media data (err %d)",
                  err);

            return err;
        }

        HDCP_private_data[0] = 0x00;

        HDCP_private_data[1] =
            (((trackIndex >> 30) & 3) << 1) | 1;

        HDCP_private_data[2] = (trackIndex >> 22) & 0xff;

        HDCP_private_data[3] =
            (((trackIndex >> 15) & 0x7f) << 1) | 1;

        HDCP_private_data[4] = (trackIndex >> 7) & 0xff;

        HDCP_private_data[5] =
            ((trackIndex & 0x7f) << 1) | 1;

        HDCP_private_data[6] = 0x00;

        HDCP_private_data[7] =
            (((inputCTR >> 60) & 0x0f) << 1) | 1;

        HDCP_private_data[8] = (inputCTR >> 52) & 0xff;

        HDCP_private_data[9] =
            (((inputCTR >> 45) & 0x7f) << 1) | 1;

        HDCP_private_data[10] = (inputCTR >> 37) & 0xff;

        HDCP_private_data[11] =
            (((inputCTR >> 30) & 0x7f) << 1) | 1;

        HDCP_private_data[12] = (inputCTR >> 22) & 0xff;

        HDCP_private_data[13] =
            (((inputCTR >> 15) & 0x7f) << 1) | 1;

        HDCP_private_data[14] = (inputCTR >> 7) & 0xff;

        HDCP_private_data[15] =
            ((inputCTR & 0x7f) << 1) | 1;

#if 0
        ALOGI("HDCP_private_data:");
        hexdump(HDCP_private_data, sizeof(HDCP_private_data));

        ABitReader br(HDCP_private_data, sizeof(HDCP_private_data));
        CHECK_EQ(br.getBits(13), 0);
        CHECK_EQ(br.getBits(2), (trackIndex >> 30) & 3);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (trackIndex >> 15) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), trackIndex & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(11), 0);
        CHECK_EQ(br.getBits(4), (inputCTR >> 60) & 0xf);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (inputCTR >> 45) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (inputCTR >> 30) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (inputCTR >> 15) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), inputCTR & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
#endif

        flags |= TSPacketizer::IS_ENCRYPTED;
    } else if (manuallyPrependSPSPPS) {
        flags |= TSPacketizer::PREPEND_SPS_PPS_TO_IDR_FRAMES;
    }

    int64_t timeUs = ALooper::GetNowUs();
    if (mPrevTimeUs < 0ll || mPrevTimeUs + 100000ll <= timeUs) {
        flags |= TSPacketizer::EMIT_PCR;
        flags |= TSPacketizer::EMIT_PAT_AND_PMT;

        mPrevTimeUs = timeUs;
    }

    mPacketizer->packetize(
            track->packetizerTrackIndex(), accessUnit, packets, flags,
            !isHDCPEncrypted ? NULL : HDCP_private_data,
            !isHDCPEncrypted ? 0 : sizeof(HDCP_private_data),
            track->isAudio() ? 2 : 0 /* numStuffingBytes */);

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::packetizeQueuedAccessUnits() {
    for (;;) {
        bool gotMoreData = false;
        for (size_t i = 0; i < mTracks.size(); ++i) {
            size_t trackIndex = mTracks.keyAt(i);
            const sp<Track> &track = mTracks.valueAt(i);

            sp<ABuffer> accessUnit = track->dequeueAccessUnit();
            if (accessUnit != NULL) {
                track->queueOutputBuffer(accessUnit);
                gotMoreData = true;
            }
        }

        if (!gotMoreData) {
            break;
        }
    }

    return OK;
}

void WifiDisplaySource::PlaybackSession::notifySessionDead() {
    // Inform WifiDisplaySource of our premature death (wish).
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionDead);
    notify->post();

    mWeAreDead = true;
}

void WifiDisplaySource::PlaybackSession::drainAccessUnits() {
    ALOGV("audio/video has %d/%d buffers ready.",
            mTracks.valueFor(1)->countQueuedOutputBuffers(),
            mTracks.valueFor(0)->countQueuedOutputBuffers());

    while (drainAccessUnit()) {
    }
}

bool WifiDisplaySource::PlaybackSession::drainAccessUnit() {
    ssize_t minTrackIndex = -1;
    int64_t minTimeUs = -1ll;

    for (size_t i = 0; i < mTracks.size(); ++i) {
        const sp<Track> &track = mTracks.valueAt(i);

        int64_t timeUs;
        if (track->hasOutputBuffer(&timeUs)) {
            if (minTrackIndex < 0 || timeUs < minTimeUs) {
                minTrackIndex = mTracks.keyAt(i);
                minTimeUs = timeUs;
            }
        }
#if SUSPEND_VIDEO_IF_IDLE
        else if (!track->isSuspended()) {
            // We still consider this track "live", so it should keep
            // delivering output data whose time stamps we'll have to
            // consider for proper interleaving.
            return false;
        }
#else
        else {
            // We need access units available on all tracks to be able to
            // dequeue the earliest one.
            return false;
        }
#endif
    }

    if (minTrackIndex < 0) {
        return false;
    }

    const sp<Track> &track = mTracks.valueFor(minTrackIndex);
    sp<ABuffer> accessUnit = track->dequeueOutputBuffer();

    sp<ABuffer> packets;
    status_t err = packetizeAccessUnit(minTrackIndex, accessUnit, &packets);

    if (err != OK) {
        notifySessionDead();
        return false;
    }

    if ((ssize_t)minTrackIndex == mVideoTrackIndex) {
        packets->meta()->setInt32("isVideo", 1);
    }
    mSender->queuePackets(minTimeUs, packets);

#if 0
    if (minTrackIndex == mVideoTrackIndex) {
        int64_t nowUs = ALooper::GetNowUs();

        // Latency from "data acquired" to "ready to send if we wanted to".
        ALOGI("[%s] latencyUs = %lld ms",
              minTrackIndex == mVideoTrackIndex ? "video" : "audio",
              (nowUs - minTimeUs) / 1000ll);
    }
#endif

    return true;
}

}  // namespace android


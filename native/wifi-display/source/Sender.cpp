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
#define LOG_TAG "Sender"
#include <utils/Log.h>

#include "Sender.h"

#include "ANetworkSession.h"
#include "TimeSeries.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

namespace android {

static size_t kMaxRTPPacketSize = 1500;
static size_t kMaxNumTSPacketsPerRTPPacket = (kMaxRTPPacketSize - 12) / 188;

Sender::Sender(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify)
    : mNetSession(netSession),
      mNotify(notify),
      mTransportMode(TRANSPORT_UDP),
      mRTPChannel(0),
      mRTCPChannel(0),
      mRTPPort(0),
      mRTPSessionID(0),
      mRTCPSessionID(0),
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
      mRTPRetransmissionSessionID(0),
      mRTCPRetransmissionSessionID(0),
#endif
      mClientRTPPort(0),
      mClientRTCPPort(0),
      mRTPConnected(false),
      mRTCPConnected(false),
      mFirstOutputBufferReadyTimeUs(-1ll),
      mFirstOutputBufferSentTimeUs(-1ll),
      mRTPSeqNo(0),
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
      mRTPRetransmissionSeqNo(0),
#endif
      mLastNTPTime(0),
      mLastRTPTime(0),
      mNumRTPSent(0),
      mNumRTPOctetsSent(0),
      mNumSRsSent(0),
      mSendSRPending(false)
#if ENABLE_RETRANSMISSION
      ,mHistoryLength(0)
#endif
#if TRACK_BANDWIDTH
      ,mFirstPacketTimeUs(-1ll)
      ,mTotalBytesSent(0ll)
#endif
#if LOG_TRANSPORT_STREAM
    ,mLogFile(NULL)
#endif
{
#if LOG_TRANSPORT_STREAM
    mLogFile = fopen("/system/etc/log.ts", "wb");
#endif
}

Sender::~Sender() {
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
    if (mRTCPRetransmissionSessionID != 0) {
        mNetSession->destroySession(mRTCPRetransmissionSessionID);
    }

    if (mRTPRetransmissionSessionID != 0) {
        mNetSession->destroySession(mRTPRetransmissionSessionID);
    }
#endif

    if (mRTCPSessionID != 0) {
        mNetSession->destroySession(mRTCPSessionID);
    }

    if (mRTPSessionID != 0) {
        mNetSession->destroySession(mRTPSessionID);
    }

#if LOG_TRANSPORT_STREAM
    if (mLogFile != NULL) {
        fclose(mLogFile);
        mLogFile = NULL;
    }
#endif
}

status_t Sender::init(
        const char *clientIP, int32_t clientRtp, int32_t clientRtcp,
        TransportMode transportMode) {
    mClientIP = clientIP;
    mTransportMode = transportMode;

    if (transportMode == TRANSPORT_TCP_INTERLEAVED) {
        mRTPChannel = clientRtp;
        mRTCPChannel = clientRtcp;
        mRTPPort = 0;
        mRTPSessionID = 0;
        mRTCPSessionID = 0;
        return OK;
    }

    mRTPChannel = 0;
    mRTCPChannel = 0;

    if (mTransportMode == TRANSPORT_TCP) {
        // XXX This is wrong, we need to allocate sockets here, we only
        // need to do this because the dongles are not establishing their
        // end until after PLAY instead of before SETUP.
        mRTPPort = 20000;
        mRTPSessionID = 0;
        mRTCPSessionID = 0;
        mClientRTPPort = clientRtp;
        mClientRTCPPort = clientRtcp;
        return OK;
    }

    int serverRtp;

    sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, id());
    sp<AMessage> rtcpNotify = new AMessage(kWhatRTCPNotify, id());

#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
    sp<AMessage> rtpRetransmissionNotify =
        new AMessage(kWhatRTPRetransmissionNotify, id());

    sp<AMessage> rtcpRetransmissionNotify =
        new AMessage(kWhatRTCPRetransmissionNotify, id());
#endif

    status_t err;
    for (serverRtp = 15550;; serverRtp += 2) {
        int32_t rtpSession;
        if (mTransportMode == TRANSPORT_UDP) {
            err = mNetSession->createUDPSession(
                        serverRtp, clientIP, clientRtp,
                        rtpNotify, &rtpSession);
        } else {
            err = mNetSession->createTCPDatagramSession(
                        serverRtp, clientIP, clientRtp,
                        rtpNotify, &rtpSession);
        }

        if (err != OK) {
            ALOGI("failed to create RTP socket on port %d", serverRtp);
            continue;
        }

        int32_t rtcpSession = 0;

        if (clientRtcp >= 0) {
            if (mTransportMode == TRANSPORT_UDP) {
                err = mNetSession->createUDPSession(
                        serverRtp + 1, clientIP, clientRtcp,
                        rtcpNotify, &rtcpSession);
            } else {
                err = mNetSession->createTCPDatagramSession(
                        serverRtp + 1, clientIP, clientRtcp,
                        rtcpNotify, &rtcpSession);
            }

            if (err != OK) {
                ALOGI("failed to create RTCP socket on port %d", serverRtp + 1);

                mNetSession->destroySession(rtpSession);
                continue;
            }
        }

#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
        if (mTransportMode == TRANSPORT_UDP) {
            int32_t rtpRetransmissionSession;

            err = mNetSession->createUDPSession(
                        serverRtp + kRetransmissionPortOffset,
                        clientIP,
                        clientRtp + kRetransmissionPortOffset,
                        rtpRetransmissionNotify,
                        &rtpRetransmissionSession);

            if (err != OK) {
                mNetSession->destroySession(rtcpSession);
                mNetSession->destroySession(rtpSession);
                continue;
            }

            CHECK_GE(clientRtcp, 0);

            int32_t rtcpRetransmissionSession;
            err = mNetSession->createUDPSession(
                        serverRtp + 1 + kRetransmissionPortOffset,
                        clientIP,
                        clientRtp + 1 + kRetransmissionPortOffset,
                        rtcpRetransmissionNotify,
                        &rtcpRetransmissionSession);

            if (err != OK) {
                mNetSession->destroySession(rtpRetransmissionSession);
                mNetSession->destroySession(rtcpSession);
                mNetSession->destroySession(rtpSession);
                continue;
            }

            mRTPRetransmissionSessionID = rtpRetransmissionSession;
            mRTCPRetransmissionSessionID = rtcpRetransmissionSession;

            ALOGI("rtpRetransmissionSessionID = %d, "
                  "rtcpRetransmissionSessionID = %d",
                  rtpRetransmissionSession, rtcpRetransmissionSession);
        }
#endif

        mRTPPort = serverRtp;
        mRTPSessionID = rtpSession;
        mRTCPSessionID = rtcpSession;

        ALOGI("rtpSessionID = %d, rtcpSessionID = %d", rtpSession, rtcpSession);
        break;
    }

    if (mRTPPort == 0) {
        return UNKNOWN_ERROR;
    }

    return OK;
}

status_t Sender::finishInit() {
    if (mTransportMode != TRANSPORT_TCP) {
        notifyInitDone();
        return OK;
    }

    sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, id());

    status_t err = mNetSession->createTCPDatagramSession(
                mRTPPort, mClientIP.c_str(), mClientRTPPort,
                rtpNotify, &mRTPSessionID);

    if (err != OK) {
        return err;
    }

    if (mClientRTCPPort >= 0) {
        sp<AMessage> rtcpNotify = new AMessage(kWhatRTCPNotify, id());

        err = mNetSession->createTCPDatagramSession(
                mRTPPort + 1, mClientIP.c_str(), mClientRTCPPort,
                rtcpNotify, &mRTCPSessionID);

        if (err != OK) {
            return err;
        }
    }

    return OK;
}

int32_t Sender::getRTPPort() const {
    return mRTPPort;
}

void Sender::queuePackets(
        int64_t timeUs, const sp<ABuffer> &tsPackets) {
    const size_t numTSPackets = tsPackets->size() / 188;

    const size_t numRTPPackets =
        (numTSPackets + kMaxNumTSPacketsPerRTPPacket - 1)
            / kMaxNumTSPacketsPerRTPPacket;

    sp<ABuffer> udpPackets = new ABuffer(
            numRTPPackets * (12 + kMaxNumTSPacketsPerRTPPacket * 188));

    udpPackets->meta()->setInt64("timeUs", timeUs);

    size_t dstOffset = 0;
    for (size_t i = 0; i < numTSPackets; ++i) {
        if ((i % kMaxNumTSPacketsPerRTPPacket) == 0) {
            static const bool kMarkerBit = false;

            uint8_t *rtp = udpPackets->data() + dstOffset;
            rtp[0] = 0x80;
            rtp[1] = 33 | (kMarkerBit ? (1 << 7) : 0);  // M-bit
            rtp[2] = (mRTPSeqNo >> 8) & 0xff;
            rtp[3] = mRTPSeqNo & 0xff;
            rtp[4] = 0x00;  // rtp time to be filled in later.
            rtp[5] = 0x00;
            rtp[6] = 0x00;
            rtp[7] = 0x00;
            rtp[8] = kSourceID >> 24;
            rtp[9] = (kSourceID >> 16) & 0xff;
            rtp[10] = (kSourceID >> 8) & 0xff;
            rtp[11] = kSourceID & 0xff;

            ++mRTPSeqNo;

            dstOffset += 12;
        }

        memcpy(udpPackets->data() + dstOffset,
               tsPackets->data() + 188 * i,
               188);

        dstOffset += 188;
    }

    udpPackets->setRange(0, dstOffset);

    sp<AMessage> msg = new AMessage(kWhatDrainQueue, id());
    msg->setBuffer("udpPackets", udpPackets);
    msg->post();

#if LOG_TRANSPORT_STREAM
    if (mLogFile != NULL) {
        fwrite(tsPackets->data(), 1, tsPackets->size(), mLogFile);
    }
#endif
}

void Sender::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRTPNotify:
        case kWhatRTCPNotify:
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
        case kWhatRTPRetransmissionNotify:
        case kWhatRTCPRetransmissionNotify:
#endif
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    int32_t errorOccuredDuringSend;
                    CHECK(msg->findInt32("send", &errorOccuredDuringSend));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    if ((msg->what() == kWhatRTPNotify
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
                            || msg->what() == kWhatRTPRetransmissionNotify
#endif
                        ) && !errorOccuredDuringSend) {
                        // This is ok, we don't expect to receive anything on
                        // the RTP socket.
                        break;
                    }

                    ALOGE("An error occurred during %s in session %d "
                          "(%d, '%s' (%s)).",
                          errorOccuredDuringSend ? "send" : "receive",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    mNetSession->destroySession(sessionID);

                    if (sessionID == mRTPSessionID) {
                        mRTPSessionID = 0;
                    } else if (sessionID == mRTCPSessionID) {
                        mRTCPSessionID = 0;
                    }
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
                    else if (sessionID == mRTPRetransmissionSessionID) {
                        mRTPRetransmissionSessionID = 0;
                    } else if (sessionID == mRTCPRetransmissionSessionID) {
                        mRTCPRetransmissionSessionID = 0;
                    }
#endif

                    notifySessionDead();
                    break;
                }

                case ANetworkSession::kWhatDatagram:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    sp<ABuffer> data;
                    CHECK(msg->findBuffer("data", &data));

                    status_t err;
                    if (msg->what() == kWhatRTCPNotify
#if ENABLE_RETRANSMISSION && RETRANSMISSION_ACCORDING_TO_RFC_XXXX
                            || msg->what() == kWhatRTCPRetransmissionNotify
#endif
                       )
                    {
                        err = parseRTCP(data);
                    }
                    break;
                }

                case ANetworkSession::kWhatConnected:
                {
                    CHECK_EQ(mTransportMode, TRANSPORT_TCP);

                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    if (sessionID == mRTPSessionID) {
                        CHECK(!mRTPConnected);
                        mRTPConnected = true;
                        ALOGI("RTP Session now connected.");
                    } else if (sessionID == mRTCPSessionID) {
                        CHECK(!mRTCPConnected);
                        mRTCPConnected = true;
                        ALOGI("RTCP Session now connected.");
                    } else {
                        TRESPASS();
                    }

                    if (mRTPConnected
                            && (mClientRTCPPort < 0 || mRTCPConnected)) {
                        notifyInitDone();
                    }
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatDrainQueue:
        {
            sp<ABuffer> udpPackets;
            CHECK(msg->findBuffer("udpPackets", &udpPackets));

            onDrainQueue(udpPackets);
            break;
        }

        case kWhatSendSR:
        {
            mSendSRPending = false;

            if (mRTCPSessionID == 0) {
                break;
            }

            onSendSR();

            scheduleSendSR();
            break;
        }
    }
}

void Sender::scheduleSendSR() {
    if (mSendSRPending || mRTCPSessionID == 0) {
        return;
    }

    mSendSRPending = true;
    (new AMessage(kWhatSendSR, id()))->post(kSendSRIntervalUs);
}

void Sender::addSR(const sp<ABuffer> &buffer) {
    uint8_t *data = buffer->data() + buffer->size();

    // TODO: Use macros/utility functions to clean up all the bitshifts below.

    data[0] = 0x80 | 0;
    data[1] = 200;  // SR
    data[2] = 0;
    data[3] = 6;
    data[4] = kSourceID >> 24;
    data[5] = (kSourceID >> 16) & 0xff;
    data[6] = (kSourceID >> 8) & 0xff;
    data[7] = kSourceID & 0xff;

    data[8] = mLastNTPTime >> (64 - 8);
    data[9] = (mLastNTPTime >> (64 - 16)) & 0xff;
    data[10] = (mLastNTPTime >> (64 - 24)) & 0xff;
    data[11] = (mLastNTPTime >> 32) & 0xff;
    data[12] = (mLastNTPTime >> 24) & 0xff;
    data[13] = (mLastNTPTime >> 16) & 0xff;
    data[14] = (mLastNTPTime >> 8) & 0xff;
    data[15] = mLastNTPTime & 0xff;

    data[16] = (mLastRTPTime >> 24) & 0xff;
    data[17] = (mLastRTPTime >> 16) & 0xff;
    data[18] = (mLastRTPTime >> 8) & 0xff;
    data[19] = mLastRTPTime & 0xff;

    data[20] = mNumRTPSent >> 24;
    data[21] = (mNumRTPSent >> 16) & 0xff;
    data[22] = (mNumRTPSent >> 8) & 0xff;
    data[23] = mNumRTPSent & 0xff;

    data[24] = mNumRTPOctetsSent >> 24;
    data[25] = (mNumRTPOctetsSent >> 16) & 0xff;
    data[26] = (mNumRTPOctetsSent >> 8) & 0xff;
    data[27] = mNumRTPOctetsSent & 0xff;

    buffer->setRange(buffer->offset(), buffer->size() + 28);
}

void Sender::addSDES(const sp<ABuffer> &buffer) {
    uint8_t *data = buffer->data() + buffer->size();
    data[0] = 0x80 | 1;
    data[1] = 202;  // SDES
    data[4] = kSourceID >> 24;
    data[5] = (kSourceID >> 16) & 0xff;
    data[6] = (kSourceID >> 8) & 0xff;
    data[7] = kSourceID & 0xff;

    size_t offset = 8;

    data[offset++] = 1;  // CNAME

    static const char *kCNAME = "someone@somewhere";
    data[offset++] = strlen(kCNAME);

    memcpy(&data[offset], kCNAME, strlen(kCNAME));
    offset += strlen(kCNAME);

    data[offset++] = 7;  // NOTE

    static const char *kNOTE = "Hell's frozen over.";
    data[offset++] = strlen(kNOTE);

    memcpy(&data[offset], kNOTE, strlen(kNOTE));
    offset += strlen(kNOTE);

    data[offset++] = 0;

    if ((offset % 4) > 0) {
        size_t count = 4 - (offset % 4);
        switch (count) {
            case 3:
                data[offset++] = 0;
            case 2:
                data[offset++] = 0;
            case 1:
                data[offset++] = 0;
        }
    }

    size_t numWords = (offset / 4) - 1;
    data[2] = numWords >> 8;
    data[3] = numWords & 0xff;

    buffer->setRange(buffer->offset(), buffer->size() + offset);
}

// static
uint64_t Sender::GetNowNTP() {
    uint64_t nowUs = ALooper::GetNowUs();

    nowUs += ((70ll * 365 + 17) * 24) * 60 * 60 * 1000000ll;

    uint64_t hi = nowUs / 1000000ll;
    uint64_t lo = ((1ll << 32) * (nowUs % 1000000ll)) / 1000000ll;

    return (hi << 32) | lo;
}

void Sender::onSendSR() {
    sp<ABuffer> buffer = new ABuffer(1500);
    buffer->setRange(0, 0);

    addSR(buffer);
    addSDES(buffer);

    if (mTransportMode == TRANSPORT_TCP_INTERLEAVED) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatBinaryData);
        notify->setInt32("channel", mRTCPChannel);
        notify->setBuffer("data", buffer);
        notify->post();
    } else {
        sendPacket(mRTCPSessionID, buffer->data(), buffer->size());
    }

    ++mNumSRsSent;
}

#if ENABLE_RETRANSMISSION
status_t Sender::parseTSFB(
        const uint8_t *data, size_t size) {
    if ((data[0] & 0x1f) != 1) {
        return ERROR_UNSUPPORTED;  // We only support NACK for now.
    }

    uint32_t srcId = U32_AT(&data[8]);
    if (srcId != kSourceID) {
        return ERROR_MALFORMED;
    }

    for (size_t i = 12; i < size; i += 4) {
        uint16_t seqNo = U16_AT(&data[i]);
        uint16_t blp = U16_AT(&data[i + 2]);

        List<sp<ABuffer> >::iterator it = mHistory.begin();
        bool foundSeqNo = false;
        while (it != mHistory.end()) {
            const sp<ABuffer> &buffer = *it;

            uint16_t bufferSeqNo = buffer->int32Data() & 0xffff;

            bool retransmit = false;
            if (bufferSeqNo == seqNo) {
                retransmit = true;
            } else if (blp != 0) {
                for (size_t i = 0; i < 16; ++i) {
                    if ((blp & (1 << i))
                        && (bufferSeqNo == ((seqNo + i + 1) & 0xffff))) {
                        blp &= ~(1 << i);
                        retransmit = true;
                    }
                }
            }

            if (retransmit) {
                ALOGI("retransmitting seqNo %d", bufferSeqNo);

#if RETRANSMISSION_ACCORDING_TO_RFC_XXXX
                sp<ABuffer> retransRTP = new ABuffer(2 + buffer->size());
                uint8_t *rtp = retransRTP->data();
                memcpy(rtp, buffer->data(), 12);
                rtp[2] = (mRTPRetransmissionSeqNo >> 8) & 0xff;
                rtp[3] = mRTPRetransmissionSeqNo & 0xff;
                rtp[12] = (bufferSeqNo >> 8) & 0xff;
                rtp[13] = bufferSeqNo & 0xff;
                memcpy(&rtp[14], buffer->data() + 12, buffer->size() - 12);

                ++mRTPRetransmissionSeqNo;

                sendPacket(
                        mRTPRetransmissionSessionID,
                        retransRTP->data(), retransRTP->size());
#else
                sendPacket(
                        mRTPSessionID, buffer->data(), buffer->size());
#endif

                if (bufferSeqNo == seqNo) {
                    foundSeqNo = true;
                }

                if (foundSeqNo && blp == 0) {
                    break;
                }
            }

            ++it;
        }

        if (!foundSeqNo || blp != 0) {
            ALOGI("Some sequence numbers were no longer available for "
                  "retransmission");
        }
    }

    return OK;
}
#endif

status_t Sender::parseRTCP(
        const sp<ABuffer> &buffer) {
    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    while (size > 0) {
        if (size < 8) {
            // Too short to be a valid RTCP header
            return ERROR_MALFORMED;
        }

        if ((data[0] >> 6) != 2) {
            // Unsupported version.
            return ERROR_UNSUPPORTED;
        }

        if (data[0] & 0x20) {
            // Padding present.

            size_t paddingLength = data[size - 1];

            if (paddingLength + 12 > size) {
                // If we removed this much padding we'd end up with something
                // that's too short to be a valid RTP header.
                return ERROR_MALFORMED;
            }

            size -= paddingLength;
        }

        size_t headerLength = 4 * (data[2] << 8 | data[3]) + 4;

        if (size < headerLength) {
            // Only received a partial packet?
            return ERROR_MALFORMED;
        }

        switch (data[1]) {
            case 200:
            case 201:  // RR
            case 202:  // SDES
            case 203:
            case 204:  // APP
                break;

#if ENABLE_RETRANSMISSION
            case 205:  // TSFB (transport layer specific feedback)
                parseTSFB(data, headerLength);
                break;
#endif

            case 206:  // PSFB (payload specific feedback)
                hexdump(data, headerLength);
                break;

            default:
            {
                ALOGW("Unknown RTCP packet type %u of size %d",
                     (unsigned)data[1], headerLength);
                break;
            }
        }

        data += headerLength;
        size -= headerLength;
    }

    return OK;
}

status_t Sender::sendPacket(
        int32_t sessionID, const void *data, size_t size) {
    return mNetSession->sendRequest(sessionID, data, size);
}

void Sender::notifyInitDone() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatInitDone);
    notify->post();
}

void Sender::notifySessionDead() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionDead);
    notify->post();
}

void Sender::onDrainQueue(const sp<ABuffer> &udpPackets) {
    static const size_t kFullRTPPacketSize =
        12 + 188 * kMaxNumTSPacketsPerRTPPacket;

    size_t srcOffset = 0;
    while (srcOffset < udpPackets->size()) {
        uint8_t *rtp = udpPackets->data() + srcOffset;

        size_t rtpPacketSize = udpPackets->size() - srcOffset;
        if (rtpPacketSize > kFullRTPPacketSize) {
            rtpPacketSize = kFullRTPPacketSize;
        }

        int64_t nowUs = ALooper::GetNowUs();
        mLastNTPTime = GetNowNTP();

        // 90kHz time scale
        uint32_t rtpTime = (nowUs * 9ll) / 100ll;

        rtp[4] = rtpTime >> 24;
        rtp[5] = (rtpTime >> 16) & 0xff;
        rtp[6] = (rtpTime >> 8) & 0xff;
        rtp[7] = rtpTime & 0xff;

        ++mNumRTPSent;
        mNumRTPOctetsSent += rtpPacketSize - 12;

        mLastRTPTime = rtpTime;

        if (mTransportMode == TRANSPORT_TCP_INTERLEAVED) {
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatBinaryData);

            sp<ABuffer> data = new ABuffer(rtpPacketSize);
            memcpy(data->data(), rtp, rtpPacketSize);

            notify->setInt32("channel", mRTPChannel);
            notify->setBuffer("data", data);
            notify->post();
        } else {
            sendPacket(mRTPSessionID, rtp, rtpPacketSize);

#if TRACK_BANDWIDTH
            mTotalBytesSent += rtpPacketSize->size();
            int64_t delayUs = ALooper::GetNowUs() - mFirstPacketTimeUs;

            if (delayUs > 0ll) {
                ALOGI("approx. net bandwidth used: %.2f Mbit/sec",
                        mTotalBytesSent * 8.0 / delayUs);
            }
#endif
        }

#if ENABLE_RETRANSMISSION
        addToHistory(rtp, rtpPacketSize);
#endif

        srcOffset += rtpPacketSize;
    }

#if 0
    int64_t timeUs;
    CHECK(udpPackets->meta()->findInt64("timeUs", &timeUs));

    ALOGI("dTimeUs = %lld us", ALooper::GetNowUs() - timeUs);
#endif
}

#if ENABLE_RETRANSMISSION
void Sender::addToHistory(const uint8_t *rtp, size_t rtpPacketSize) {
    sp<ABuffer> packet = new ABuffer(rtpPacketSize);
    memcpy(packet->data(), rtp, rtpPacketSize);

    unsigned rtpSeqNo = U16_AT(&rtp[2]);
    packet->setInt32Data(rtpSeqNo);

    mHistory.push_back(packet);
    ++mHistoryLength;

    if (mHistoryLength > kMaxHistoryLength) {
        mHistory.erase(mHistory.begin());
        --mHistoryLength;
    }
}
#endif

}  // namespace android


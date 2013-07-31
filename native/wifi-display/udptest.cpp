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

//#define LOG_NEBUG 0
#define LOG_TAG "udptest"
#include <utils/Log.h>

#include "ANetworkSession.h"

#include <binder/ProcessState.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/Utils.h>

namespace android {

struct TestHandler : public AHandler {
    TestHandler(const sp<ANetworkSession> &netSession);

    void startServer(unsigned localPort);
    void startClient(const char *remoteHost, unsigned remotePort);

protected:
    virtual ~TestHandler();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatStartServer,
        kWhatStartClient,
        kWhatUDPNotify,
        kWhatSendPacket,
    };

    sp<ANetworkSession> mNetSession;

    bool mIsServer;
    bool mConnected;
    int32_t mUDPSession;
    uint32_t mSeqNo;
    double mTotalTimeUs;
    int32_t mCount;

    void postSendPacket(int64_t delayUs = 0ll);

    DISALLOW_EVIL_CONSTRUCTORS(TestHandler);
};

TestHandler::TestHandler(const sp<ANetworkSession> &netSession)
    : mNetSession(netSession),
      mIsServer(false),
      mConnected(false),
      mUDPSession(0),
      mSeqNo(0),
      mTotalTimeUs(0.0),
      mCount(0) {
}

TestHandler::~TestHandler() {
}

void TestHandler::startServer(unsigned localPort) {
    sp<AMessage> msg = new AMessage(kWhatStartServer, id());
    msg->setInt32("localPort", localPort);
    msg->post();
}

void TestHandler::startClient(const char *remoteHost, unsigned remotePort) {
    sp<AMessage> msg = new AMessage(kWhatStartClient, id());
    msg->setString("remoteHost", remoteHost);
    msg->setInt32("remotePort", remotePort);
    msg->post();
}

void TestHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStartClient:
        {
            AString remoteHost;
            CHECK(msg->findString("remoteHost", &remoteHost));

            int32_t remotePort;
            CHECK(msg->findInt32("remotePort", &remotePort));

            sp<AMessage> notify = new AMessage(kWhatUDPNotify, id());

            CHECK_EQ((status_t)OK,
                     mNetSession->createUDPSession(
                         0 /* localPort */,
                         remoteHost.c_str(),
                         remotePort,
                         notify,
                         &mUDPSession));

            postSendPacket();
            break;
        }

        case kWhatStartServer:
        {
            mIsServer = true;

            int32_t localPort;
            CHECK(msg->findInt32("localPort", &localPort));

            sp<AMessage> notify = new AMessage(kWhatUDPNotify, id());

            CHECK_EQ((status_t)OK,
                     mNetSession->createUDPSession(
                         localPort, notify, &mUDPSession));

            break;
        }

        case kWhatSendPacket:
        {
            char buffer[12];
            memset(buffer, 0, sizeof(buffer));

            buffer[0] = mSeqNo >> 24;
            buffer[1] = (mSeqNo >> 16) & 0xff;
            buffer[2] = (mSeqNo >> 8) & 0xff;
            buffer[3] = mSeqNo & 0xff;
            ++mSeqNo;

            int64_t nowUs = ALooper::GetNowUs();
            buffer[4] = nowUs >> 56;
            buffer[5] = (nowUs >> 48) & 0xff;
            buffer[6] = (nowUs >> 40) & 0xff;
            buffer[7] = (nowUs >> 32) & 0xff;
            buffer[8] = (nowUs >> 24) & 0xff;
            buffer[9] = (nowUs >> 16) & 0xff;
            buffer[10] = (nowUs >> 8) & 0xff;
            buffer[11] = nowUs & 0xff;

            CHECK_EQ((status_t)OK,
                     mNetSession->sendRequest(
                         mUDPSession, buffer, sizeof(buffer)));

            postSendPacket(20000ll);
            break;
        }

        case kWhatUDPNotify:
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

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("An error occurred in session %d (%d, '%s/%s').",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    mNetSession->destroySession(sessionID);
                    break;
                }

                case ANetworkSession::kWhatDatagram:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    sp<ABuffer> data;
                    CHECK(msg->findBuffer("data", &data));

                    if (mIsServer) {
                        if (!mConnected) {
                            AString fromAddr;
                            CHECK(msg->findString("fromAddr", &fromAddr));

                            int32_t fromPort;
                            CHECK(msg->findInt32("fromPort", &fromPort));

                            CHECK_EQ((status_t)OK,
                                     mNetSession->connectUDPSession(
                                         mUDPSession, fromAddr.c_str(), fromPort));

                            mConnected = true;
                        }

                        int64_t nowUs = ALooper::GetNowUs();

                        sp<ABuffer> buffer = new ABuffer(data->size() + 8);
                        memcpy(buffer->data(), data->data(), data->size());

                        uint8_t *ptr = buffer->data() + data->size();

                        *ptr++ = nowUs >> 56;
                        *ptr++ = (nowUs >> 48) & 0xff;
                        *ptr++ = (nowUs >> 40) & 0xff;
                        *ptr++ = (nowUs >> 32) & 0xff;
                        *ptr++ = (nowUs >> 24) & 0xff;
                        *ptr++ = (nowUs >> 16) & 0xff;
                        *ptr++ = (nowUs >> 8) & 0xff;
                        *ptr++ = nowUs & 0xff;

                        CHECK_EQ((status_t)OK,
                                 mNetSession->sendRequest(
                                     mUDPSession, buffer->data(), buffer->size()));
                    } else {
                        CHECK_EQ(data->size(), 20u);

                        uint32_t seqNo = U32_AT(data->data());
                        int64_t t1 = U64_AT(data->data() + 4);
                        int64_t t2 = U64_AT(data->data() + 12);

                        int64_t t3;
                        CHECK(data->meta()->findInt64("arrivalTimeUs", &t3));

#if 0
                        printf("roundtrip seqNo %u, time = %lld us\n",
                               seqNo, t3 - t1);
#else
                        mTotalTimeUs += t3 - t1;
                        ++mCount;
                        printf("avg. roundtrip time %.2f us\n", mTotalTimeUs / mCount);
#endif
                    }
                    break;
                }

                default:
                    TRESPASS();
            }

            break;
        }

        default:
            TRESPASS();
    }
}

void TestHandler::postSendPacket(int64_t delayUs) {
    (new AMessage(kWhatSendPacket, id()))->post(delayUs);
}

}  // namespace android

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s -c host[:port]\tconnect to test server\n"
            "           -l            \tcreate a test server\n",
            me);
}

int main(int argc, char **argv) {
    using namespace android;

    ProcessState::self()->startThreadPool();

    int32_t localPort = -1;
    int32_t connectToPort = -1;
    AString connectToHost;

    int res;
    while ((res = getopt(argc, argv, "hc:l:")) >= 0) {
        switch (res) {
            case 'c':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    connectToHost = optarg;
                    connectToPort = 49152;
                } else {
                    connectToHost.setTo(optarg, colonPos - optarg);

                    char *end;
                    connectToPort = strtol(colonPos + 1, &end, 10);

                    if (*end != '\0' || end == colonPos + 1
                            || connectToPort < 1 || connectToPort > 65535) {
                        fprintf(stderr, "Illegal port specified.\n");
                        exit(1);
                    }
                }
                break;
            }

            case 'l':
            {
                char *end;
                localPort = strtol(optarg, &end, 10);

                if (*end != '\0' || end == optarg
                        || localPort < 1 || localPort > 65535) {
                    fprintf(stderr, "Illegal port specified.\n");
                    exit(1);
                }
                break;
            }

            case '?':
            case 'h':
                usage(argv[0]);
                exit(1);
        }
    }

    if (localPort < 0 && connectToPort < 0) {
        fprintf(stderr,
                "You need to select either client or server mode.\n");
        exit(1);
    }

    sp<ANetworkSession> netSession = new ANetworkSession;
    netSession->start();

    sp<ALooper> looper = new ALooper;

    sp<TestHandler> handler = new TestHandler(netSession);
    looper->registerHandler(handler);

    if (localPort >= 0) {
        handler->startServer(localPort);
    } else {
        handler->startClient(connectToHost.c_str(), connectToPort);
    }

    looper->start(true /* runOnCallingThread */);

    return 0;
}


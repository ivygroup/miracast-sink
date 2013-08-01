#include "SinkPlayer.h"

#include "ANetworkSession.h"
#include "sink/WifiDisplaySink.h"

namespace android {

SinkPlayer::SinkPlayer(const char *host, int32_t port)
    : mLooper(new ALooper),
      mNetSession(new ANetworkSession),
      mSink(new WifiDisplaySink(mNetSession)) {
    mNetSession->start();
    mLooper->setName("sink_player");
    mLooper->registerHandler(mSink);
    mSink->start(host, port);
    mLooper->start(true /* runOnCallingThread */);
}

SinkPlayer::~SinkPlayer() {
}

status_t SinkPlayer::dispose() {
    /*mSink->stop();

    mLooper->stop();
    mNetSession->stop();*/

    return OK;
}

}  // namespace android

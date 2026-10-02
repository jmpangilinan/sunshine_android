#pragma once

#include "stream.h"
#include "input.h"

namespace sunshine_callbacks {
    void hostListenerReady(unsigned listener);
    void hostStartupFailed(const std::string &reason);
    void initializeVideoCapabilities();
    bool supportsVideo(const video::config_t &config);
    void callJavaOnPinRequested();

    void captureVideoLoop(void *channel_data, safe::mail_t mail, const video::config_t &config,
                          const safe::mail_raw_t::queue_t<video::packet_t> &packets,
                          std::shared_ptr<input::input_t> &input);

    void captureAudioLoop(void *channel_data, safe::mail_t mail, const audio::config_t &config);


}
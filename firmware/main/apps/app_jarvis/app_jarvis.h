/*
 * Man on the Moon - ruimte- en audioweergave voor de StopWatch
 */
#pragma once
#include "view/jarvis_view.h"
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <memory>

class AppJarvis : public mooncake::AppAbility {
public:
    AppJarvis();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<input::KeyManager> _keyManager;
    std::unique_ptr<jarvis::JarvisView> _view;
    jarvis::State _state = jarvis::State::Idle;
    uint32_t _nextFrameAt = 0;
    uint32_t _decSum = 0, _pushSum = 0;

    bool _micWas        = true;
    int _touchCount     = 0;
    float _tx0 = 0, _ty0 = 0, _drag = 0;      // begin van de aanraking en hoe ver de vinger bewoog
    uint32_t _downAt = 0;
    bool _wasTouched    = false;
    uint32_t _stateSince = 0;
    uint32_t _fpsFrames  = 0;
    uint32_t _fpsSince   = 0;

    void setState(jarvis::State s);
};

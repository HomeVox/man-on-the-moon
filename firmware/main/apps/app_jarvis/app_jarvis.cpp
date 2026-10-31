/*
 * Man on the Moon - ruimte- en audioweergave voor de StopWatch
 *
 * Planeten, audiogolf en sterrenhemel; de microfoon stuurt de beelden.
 * Bediening via knoppen, aanraking en Home Assistant (ha/ha_ctl).
 */
#include "app_jarvis.h"
#include <hal/hal.h>
#include "ha/ha_ctl.h"
#include <mooncake_log.h>
#include <cmath>
#include <ctime>
#include <vector>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

using namespace mooncake;
using namespace jarvis;

// Testbeeld in plaats van de animatie (om vervorming op het scherm te zien)
static constexpr bool kTestPattern = false;

// Astronautenzin bij een tik op een bol: willekeurig één van de vier van die planeet, nooit twee keer dezelfde
// achter elkaar. Staan in de flash-partitie "stemmen" (tools/stemmen.py), 22050 Hz; de speaker speelt 44100 Hz.
// Deep sleep: knop A of B maakt hem weer wakker (dan start hij opnieuw op, zonder opstartgeluid).
static void slapen()
{
    auto& hal = GetHAL();
    ctl::goingToSleep();
    hal.prepareDeepSleep();
    // knop A (GPIO2) en B (GPIO1), laag = ingedrukt; als RTC-pin met eigen pull-up, die blijft in deep sleep
    for (gpio_num_t p : {GPIO_NUM_2, GPIO_NUM_1}) {
        rtc_gpio_init(p);
        rtc_gpio_set_direction(p, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_dis(p);
        rtc_gpio_pullup_en(p);
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // pull-ups aanhouden
    for (int i = 0; i < 100 && (!rtc_gpio_get_level(GPIO_NUM_2) || !rtc_gpio_get_level(GPIO_NUM_1)); i++)
        vTaskDelay(pdMS_TO_TICKS(20));   // eerst knop loslaten, anders meteen weer wakker
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 0);                       // knop A
    esp_sleep_enable_ext1_wakeup(1ULL << 1, ESP_EXT1_WAKEUP_ANY_LOW);  // knop B
    esp_deep_sleep_start();
}

static void speelStem(int planeet)
{
    static const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "stemmen");
    if (!part || planeet < 0 || planeet > 3) return;
    uint32_t kop[3];
    if (esp_partition_read(part, 0, kop, sizeof(kop)) != ESP_OK || kop[0] != 0x4D455453 || kop[1] != 16) return;   // "STEM"
    if (kop[2] * 2 != (uint32_t)GetHAL().getAudioSampleRate()) return;
    static int laatste[4] = {-1, -1, -1, -1};
    int k;
    do k = (int)(esp_random() % 4); while (k == laatste[planeet]);
    laatste[planeet] = k;
    uint32_t ent[2];
    if (esp_partition_read(part, 12 + 8 * (planeet * 4 + k), ent, sizeof(ent)) != ESP_OK || ent[1] == 0 || ent[1] > 200000) return;
    std::vector<int16_t> in(ent[1]);
    if (esp_partition_read(part, 12 + 8 * kop[1] + ent[0] * 2, in.data(), ent[1] * 2) != ESP_OK) return;
    std::vector<int16_t> out(ent[1] * 2);   // 2x: tussen elke twee samples het gemiddelde
    for (uint32_t n = 0; n < ent[1]; n++) {
        out[2 * n]     = in[n];
        out[2 * n + 1] = (int16_t)((in[n] + in[n + 1 < ent[1] ? n + 1 : n]) / 2);
    }
    GetHAL().audioPlay(out, true);
}

AppJarvis::AppJarvis()
{
    setAppInfo().name = "Jarvis";
}

void AppJarvis::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create, wakker door {}", (int)esp_sleep_get_wakeup_cause());
    for (gpio_num_t p : {GPIO_NUM_2, GPIO_NUM_1}) {   // knoppen terug van RTC-pin (slapen) naar gewone pin
        rtc_gpio_deinit(p);
        gpio_set_pull_mode(p, GPIO_PULLUP_ONLY);
    }
}

void AppJarvis::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _keyManager = std::make_unique<input::KeyManager>();

    {
        LvglLockGuard lock;
        GetHAL().bootLogo.reset();
    }

    GetHAL().setMicActive(true);
    ctl::begin();  // wifi + HA-verbinding

    // LVGL stilzetten: de view tekent zelf en stuurt beelden rechtstreeks naar het scherm.
    // (Voorgerenderde JPEG-lussen via AnimPlayer waren te traag: ~140 ms decoderen per beeld.)
    GetHAL().stopLvglUpdate();
    _view = std::make_unique<JarvisView>();
    if (!_view->create()) {
        mclog::tagError(getAppInfo().name, "view maken mislukt");
        _view.reset();
    }
    if (kTestPattern && _view) _view->showTestPattern();
    if (_view) _view->startAsWave();
    _stateSince = GetHAL().millis();
    _fpsSince   = _stateSince;
}

void AppJarvis::setState(State s)
{
    mclog::tagInfo(getAppInfo().name, "toestand: {}", stateName(s));
    _state = s;
    if (_view) _view->setState(s);
    _stateSince = GetHAL().millis();
}

void AppJarvis::onRunning()
{
    auto ev = _keyManager->update();
    auto& hal = GetHAL();
    uint32_t now = hal.millis();

    // Aanraking: slepen vervormt de draden; een korte tik (zonder te bewegen) of knop A = luisteren.
    // Een aanraking telt pas na twee metingen achter elkaar (negeert losse stoorpulsen).
    // Nachtklok: tussen "van" en "tot" (HA) staat het scherm uit. Een tik of knop zet het 30 s aan met de aarde en de
    // klok in amber, gedimd; die tik of knop doet verder niets.
    static uint32_t aanTot = 0;
    static bool slikTik = false;
    bool nacht = false;
    if (ctl::nachtklok() && JarvisView::tijdBekend()) {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        const int van = ctl::nachtVan(), tot = ctl::nachtTot(), u = tm.tm_hour;
        nacht = van < tot ? (u >= van && u < tot) : (van > tot && (u >= van || u < tot));
    }
    if (ctl::nachtNu()) nacht = true;   // nachtstand met de hand: ook buiten de tijden (en zonder internettijd)
    const bool nachtBeeld = nacht;
    if (nacht && ev != input::KeyEvent::None) { aanTot = now + 30000; ev = input::KeyEvent::None; }

    auto tp = hal.getTouchPoint();
    bool wasTouched = _touchCount >= 2;
    if (tp.num > 0) _touchCount++; else _touchCount = 0;
    bool touched = _touchCount >= 2;
    if (touched) {
        if (!wasTouched && nacht) { aanTot = now + 30000; slikTik = true; }
        if (!wasTouched) { _tx0 = (float)tp.x; _ty0 = (float)tp.y; _drag = 0; _downAt = now; hal.vibrate(20, 50); }
        float mv = fabsf(tp.x - _tx0) + fabsf(tp.y - _ty0);
        if (mv > _drag) _drag = mv;
        if (_view) _view->setTouch(true, (float)tp.x, (float)tp.y);
    } else {
        if (wasTouched && slikTik) slikTik = false;
        else if (wasTouched && _drag < 25.0f && now - _downAt < 600 && ctl::mode() >= 3) speelStem(ctl::mode() - 3);
        if (_view) _view->setTouch(false, 0, 0);
    }
    if (ev == input::KeyEvent::GoPrevious) ctl::cycleMode();     // knop A: volgende planeet
    if (ev == input::KeyEvent::GoNext) ctl::audioMode();         // knop B: Audio, Cirkel, Water

    const bool schermUit = nacht && now >= aanTot;   // nachtklok zonder aanraking: scherm uit, niets tekenen
    {   // volume uit HA
        static int vol = -1;
        if (ctl::volume() != vol) { vol = ctl::volume(); hal.setSpeakerVolume(vol, false); }
        static int hel = -1;   // helderheid uit HA
        const int helDoel = schermUit ? 0 : (nacht ? 10 : ctl::helderheid());   // 's nachts uit of gedimd
        if (helDoel != hel) { hel = helDoel; hal.setBackLightBrightness(hel, false); }
    }
    // na 2 minuten zonder aanraking, knop of modusknop in deep sleep (schakelaar in HA); knop Slapen in HA = meteen
    {
        static uint32_t actief = 0, usbCheck = 0;
        static int modus = -1;
        static bool usb = false;
        if (now - usbCheck > 1000) { usbCheck = now; usb = hal.isBatteryCharging(); }   // aan USB: niet vanzelf slapen
        if (actief == 0 || usb || nacht || touched || ev != input::KeyEvent::None || ctl::mode() != modus) { actief = now; modus = ctl::mode(); }
        if (ctl::takeSleepNow() || (ctl::autoSleep() && now - actief > 120000)) slapen();
    }

    // De animatie volgt altijd de microfoon
    if (_state != State::Listening) setState(State::Listening);

    if (_view && !kTestPattern && !schermUit) {
        float level = 0.0f;
        {   // HA-schuifjes: heftigheid en microfoon aan/uit
            bool mic = ctl::micOn();
            if (mic != _micWas) { hal.setMicActive(mic); _micWas = mic; }
            // Audio iets rustiger: op 50 nu 0,67 (was 1); de schuif loopt tot 1,33
            _view->setGain(ctl::intensity() / (ctl::mode() == 0 ? 75.0f : 50.0f));
            _view->setMelkweg(ctl::melkweg() / 50.0f);
            // gevoeligheid: 50 = normaal, 100 = 4x, 0 = kwart (software, na de automatische normalisatie)
            float sens = powf(2.0f, (ctl::sensitivity() - 50.0f) / 25.0f);
            level = mic ? fminf(1.0f, hal.getMicLevel() * sens) : 0.0f;
            float bands[20] = {};
            if (mic) { hal.getMicBands(bands); for (float& b : bands) b = fminf(1.0f, b * sens); }
            _view->setMode(nachtBeeld ? 3 : ctl::mode());   // 's nachts de aarde
            _view->setKlok(nachtBeeld || ctl::klokOn());
            _view->setNacht(nachtBeeld);
            _view->setBands(bands);
            float notes[12] = {};
            if (mic) hal.getMicNotes(notes);
            _view->setNotes(notes);
            hal.updateImuData();   // waterpas: het beeld draait tegen het draaien van het apparaat in (geen diepte of perspectief)
            const auto& imu = hal.getImuData();
            _view->setTiltStrength(1.0f);
            _view->setTilt(imu.accelX, imu.accelY, imu.accelZ);
        }
        _view->setLevel(level);
        _view->tick(now);
        _fpsFrames++;
        _decSum += _view->lastRenderMs();
        _pushSum += _view->lastPushMs();
    }

    if (now - _fpsSince >= 5000) {
        uint32_t n = _fpsFrames ? _fpsFrames : 1;
        mclog::tagInfo(getAppInfo().name, "{} fps, toestand {}, beeld {} ms, tonen {} ms, punten {} ms, tekenen {} ms",
                       _fpsFrames * 1000 / (now - _fpsSince), stateName(_state), _decSum / n, _pushSum / n, _view ? _view->lastPointsMs() : 0, _view ? _view->lastWorkMs() : 0);
        _fpsFrames = _decSum = _pushSum = 0;
        _fpsSince = now;
    }

    hal.delay(schermUit ? 30 : 2);
}

void AppJarvis::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    _keyManager.reset();
    _view.reset();
    GetHAL().startLvglUpdate();
}

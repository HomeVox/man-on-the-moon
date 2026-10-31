/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/settings/settings.h"
#include <mooncake_log.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <memory>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <algorithm>

static const std::string_view _tag = "HAL-Display";

/* -------------------------------------------------------------------------- */
/*                               Amoled display                               */
/* -------------------------------------------------------------------------- */
static constexpr gpio_num_t cfg_pin_sclk = GPIO_NUM_40;
static constexpr gpio_num_t cfg_pin_io0  = GPIO_NUM_41;
static constexpr gpio_num_t cfg_pin_io1  = GPIO_NUM_42;
static constexpr gpio_num_t cfg_pin_io2  = GPIO_NUM_46;
static constexpr gpio_num_t cfg_pin_io3  = GPIO_NUM_45;
static constexpr gpio_num_t cfg_pin_cs   = GPIO_NUM_39;
static constexpr gpio_num_t cfg_pin_te   = GPIO_NUM_38;
static constexpr gpio_num_t cfg_pin_rst  = GPIO_NUM_NC;

class Panel_CO5300 : public lgfx::Panel_AMOLED {
public:
    Panel_CO5300(void)
    {
        _cfg.memory_width = _cfg.panel_width = 480;
        _cfg.memory_height = _cfg.panel_height = 480;
        _write_depth                           = lgfx::color_depth_t::rgb565_2Byte;
        _read_depth                            = lgfx::color_depth_t::rgb565_2Byte;
    }

    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            0x11, 0 + CMD_INIT_DELAY,
            150,  // Sleep out
            0xC4, 1,
            0x80, 0x35,
            1,    0x80,
            0x44, 2,
            0x01, 0xD2,  // Tear Effect Line = 0x1D2 == 466
            0x53, 1,
            0x20, 0x20,
            0,    0x36,
            1,    0,
            0x51, 1,
            0xA0, 0x29,
            0,    0xff,
            0xff  // end
        };
        switch (listno) {
            case 0:
                return list0;
            default:
                return nullptr;
        }
    }
};

class M5StopWatch : public M5GFX {
    lgfx::Bus_SPI _bus_instance;
    Panel_CO5300 _panel_instance;

public:
    M5StopWatch(void)
    {
    }

    // static constexpr int in_i2c_port                   = 0;  // I2C_NUM_0

    bool init_impl(bool use_reset, bool use_clear) override
    {
        {
            auto cfg = _bus_instance.config();

            cfg.freq_write = 80000000;
            cfg.freq_read  = 10000000;  // irrelevant

            cfg.pin_sclk = cfg_pin_sclk;
            cfg.pin_io0  = cfg_pin_io0;
            cfg.pin_io1  = cfg_pin_io1;
            cfg.pin_io2  = cfg_pin_io2;
            cfg.pin_io3  = cfg_pin_io3;

            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;  // SPI_MODE0;
            cfg.spi_3wire   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg         = _panel_instance.config();
            cfg.pin_rst      = cfg_pin_rst;
            cfg.pin_cs       = cfg_pin_cs;
            cfg.panel_width  = 468;
            cfg.panel_height = 466;
            cfg.offset_x     = 6;
            cfg.offset_y     = 0;

            cfg.readable = false;

            _panel_instance.config(cfg);
        }

        setPanel(&_panel_instance);

        lgfx::pinMode(cfg_pin_te, lgfx::pin_mode_t::input_pullup);
        // lgfx::i2c::init(in_i2c_port);

        // io_expander.digitalWrite(PY32_L3B_EN_PIN, 1);
        // io_expander.digitalWrite(PY32_OLED_RST_PIN, 1);

        if (!LGFX_Device::init_impl(use_reset, use_clear)) return false;

        // Kantoor: geen automatisch versturen per strook; het hele beeld gaat in één keer
        // naar het scherm als LVGL klaar is (zie refr_event_cb), dus geen zichtbare opbouw
        // Jarvis: geen framebuffer-paneel. LVGL en de animatie schrijven rechtstreeks naar het paneel
        // (wisselen tussen beide modi liet het scherm donker na een gegenereerd scherm). Het paneel
        // wil even start/einde van schrijfgebieden: zie rounder_cb bij lvgl_init.

        _panel_instance.setBrightness(128);

        return true;
    }

    bool enableFrameBuffer(bool auto_display = false)
    {
        if (_panel_instance.initPanelFb()) {
            auto fbPanel = _panel_instance.getPanelFb();
            if (fbPanel) {
                fbPanel->setBus(&_bus_instance);
                fbPanel->setAutoDisplay(auto_display);
                setPanel(fbPanel);
                return true;
            }
        }
        return false;
    }

    void disableFrameBuffer()
    {
        auto fbPanel = _panel_instance.getPanelFb();
        if (fbPanel) {
            _panel_instance.deinitPanelFb();
            setPanel(&_panel_instance);
        }
    }

    void setBrightness(uint8_t brightness)
    {
        _panel_instance.setBrightness(brightness);
    }
};

static std::unique_ptr<M5StopWatch> _display;
static std::unique_ptr<LGFX_Sprite> _canvas;

void Hal::display_init()
{
    mclog::tagInfo(_tag, "display init");

    _display = std::make_unique<M5StopWatch>();
    if (!_display->init()) {
        mclog::tagError(_tag, "display init failed");
        _display.reset();
    } else {
        // Kantoor: één keer het hele scherm versturen, zodat de DMA-buffers meteen de volle
        // breedte krijgen en daarna nooit meer geheugen hoeven aan te vragen
        _display->fillScreen(TFT_BLACK);
        _display->display();
    }

    // mclog::tagInfo(_tag, "create full screen canvas");
    // _canvas = std::make_unique<LGFX_Sprite>(_display.get());
    // _canvas->setPsram(true);
    // if (!_canvas->createSprite(_display->width(), _display->height())) {
    //     mclog::tagError(_tag, "canvas init failed");
    //     _canvas.reset();
    // }

    // Load brightness from settings
    auto brightness = getBackLightBrightness(true);
    setBackLightBrightness(brightness, false);
}

LGFX_Device &Hal::getDisplay()
{
    return *_display;
}

LGFX_Sprite &Hal::getCanvas()
{
    return *_canvas;
}

void Hal::updateCanvas()
{
    _canvas->pushSprite(0, 0);
}

void Hal::pushFrame(const uint16_t* rgb565)
{
    if (!_display) return;
    // rgb565 is een beeld van 466 x 466. Het paneel kan breder zijn (panel_width 468): kopieer dan
    // elke regel in de breedte van het scherm, anders schuift elke regel op (schuin rooster).
    const uint32_t w = _display->width(), h = _display->height();
    const uint32_t srcW = w;  // buffer heeft de breedte van het scherm (zie JarvisView)
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGI("PUSH", "scherm %lux%lu, beeld %lux466", (unsigned long)w, (unsigned long)h, (unsigned long)srcW);
    }
    // Stroken van 28 regels, zoals lvgl_flush_cb
    const uint32_t stripRows = 28, chunk = 8192;
    static uint16_t* strip = nullptr;
    if (w != srcW && !strip) strip = (uint16_t*)heap_caps_malloc(w * stripRows * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t padL = (w > srcW) ? (w - srcW) / 2 : 0;
    for (uint32_t y = 0; y < h; y += stripRows) {
        uint32_t rows = std::min<uint32_t>(stripRows, h - y);
        const lgfx::rgb565_t* src;
        if (w == srcW || !strip) {
            src = (const lgfx::rgb565_t*)(rgb565 + (size_t)y * srcW);
        } else {
            for (uint32_t r = 0; r < rows; r++) {
                uint16_t* d = strip + r * w;
                const uint16_t* sp = rgb565 + (size_t)(y + r) * srcW;
                for (uint32_t x = 0; x < padL; x++) d[x] = 0;
                memcpy(d + padL, sp, srcW * 2);
                for (uint32_t x = padL + srcW; x < w; x++) d[x] = 0;
            }
            src = (const lgfx::rgb565_t*)strip;
        }
        uint32_t pixels = w * rows;
        _display->startWrite();
        _display->setAddrWindow(0, y, w, rows);
        for (uint32_t off = 0; off < pixels; off += chunk) {
            _display->writePixels(src + off, std::min<uint32_t>(chunk, pixels - off));
        }
        _display->endWrite();
    }
    _display->display();  // framebuffer-paneel: nu in een keer naar het scherm
}

static bool _fbOff = true;  // nooit een framebuffer-paneel

void Hal::pushFrameStart(const uint16_t* rgb565)
{
    if (!_display) return;
    if (!_fbOff) {
        _display->disableFrameBuffer();  // rechtstreeks naar het paneel, geen kopie via PSRAM
        _fbOff = true;
    }
    _display->startWrite();
    _display->setAddrWindow(0, 0, _display->width(), _display->height());
    _display->writePixelsDMA((const lgfx::rgb565_t*)rgb565, _display->width() * _display->height());
}

void Hal::displayUseFramebuffer(bool on)
{
    (void)on;  // niet meer nodig: er is geen framebuffer-paneel
}

void Hal::displayRecover()
{
    if (!_display) return;
    _display->waitDMA();
    _display->waitDisplay();
    auto* panel = _display->getPanel();
    if (panel) panel->init(false);                         // stuurt de init-commando's (pixelformaat, display aan) opnieuw
    _display->setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
    setBackLightBrightness(_bl_brightness);                // helderheid zoals die was
    _display->fillScreen(0);
    ESP_LOGI("DISPLAY", "paneel hersteld");
}

void Hal::pushFrameWait()
{
    if (!_display) return;
    _display->waitDMA();
    _display->endWrite();
}

void Hal::setBackLightBrightness(int brightness, bool saveToSettings)
{
    _bl_brightness = uitk::clamp(brightness, 0, 100);

    int set_target = uitk::map_range(_bl_brightness, 0, 100, 0, 255);
    _display->setBrightness(set_target);

    if (saveToSettings) {
        Settings settings(std::string(Hal::SettingsNs), true);
        settings.SetInt("bl_lev", _bl_brightness);
        mclog::tagInfo(_tag, "brightness saved to settings: {}", _bl_brightness);
    }
}

int Hal::getBackLightBrightness(bool loadFromSettings)
{
    if (loadFromSettings) {
        Settings settings(std::string(Hal::SettingsNs), false);
        _bl_brightness = settings.GetInt("bl_lev", 80);
        _bl_brightness = uitk::clamp(_bl_brightness, 10, 100);
        mclog::tagInfo(_tag, "brightness loaded from settings: {}", _bl_brightness);
    }
    return _bl_brightness;
}

/* -------------------------------------------------------------------------- */
/*                                  Touchpad                                  */
/* -------------------------------------------------------------------------- */
#include "drivers/cst820/cst820.h"

static std::unique_ptr<Cst820> _cst820;

void Hal::touchpad_init()
{
    mclog::tagInfo(_tag, "touchpad init");

    ioe_tp_reset();

    _cst820 = std::make_unique<Cst820>();
    if (!_cst820->begin(i2c_bus_get_internal_bus_handle(_i2c_bus))) {
        mclog::tagError(_tag, "touchpad init failed");
        _cst820.reset();
    }
}

Hal::TouchPoint Hal::getTouchPoint()
{
    Hal::TouchPoint point;
    if (_cst820 && _cst820->read()) {
        point.num = _cst820->getFingerNum();
        if (point.num > 0) {
            point.x = _cst820->getX();
            point.y = _cst820->getY();
        }
    }
    return point;
}

/* -------------------------------------------------------------------------- */
/*                                    Lvgl                                    */
/* -------------------------------------------------------------------------- */
// https://github.com/m5stack/lv_m5_emulator/blob/main/src/utility/lvgl_port_m5stack.cpp
#include <cstdlib>  // for aligned_alloc
#include <cstring>  // for memset
#include <lvgl.h>
#include <atomic>
#include <algorithm>
#include <esp_timer.h>
#include <esp_log.h>

static SemaphoreHandle_t xGuiSemaphore;
static std::atomic<bool> _lvgl_update_enabled = false;

#define LV_BUFFER_LINE 120

static void lvgl_tick_timer(void *arg)
{
    (void)arg;
    lv_tick_inc(10);
}

// Kantoor: meten waar de tijd heen gaat (op te vragen via /stats)
static uint32_t _refrUs       = 0;  // tekentijd in de huidige ronde
static uint32_t _flushRoundUs = 0;  // waarvan versturen
static int64_t _refrStart     = 0;

static void refr_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        _refrStart = esp_timer_get_time();
    } else {
        // Beeld compleet in het framebuffer: nu in één keer naar het scherm
        int64_t t    = esp_timer_get_time();
        M5GFX* gfx   = (M5GFX*)lv_display_get_driver_data(lv_display_get_default());
        if (gfx) gfx->display();
        _flushRoundUs += (uint32_t)(esp_timer_get_time() - t);
        if (_refrStart) {
            _refrUs += (uint32_t)(esp_timer_get_time() - _refrStart);
            _refrStart = 0;
        }
    }
}

static struct {
    uint32_t frames     = 0;  // rondes waarin echt iets getekend werd (> 1 ms)
    uint64_t handlerUs  = 0;  // tijd in lv_timer_handler (rekenen + versturen)
    uint32_t maxUs      = 0;
    uint64_t flushUs    = 0;  // waarvan versturen naar het scherm
    uint64_t pixels     = 0;
    int64_t sinceUs     = 0;
} _stats;

static void lvgl_rtos_task(void *pvParameter)
{
    (void)pvParameter;
    while (1) {
        if (_lvgl_update_enabled && pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            int64_t t0 = esp_timer_get_time();
            _refrUs    = 0;
            _flushRoundUs = 0;
            lv_timer_handler();
            uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            if (dt > 50000) {
                // Trage ronde: hoeveel was tekenen (incl. versturen), de rest is aanraken/animaties/timers
                ESP_LOGW("TEKEN", "trage ronde %lu ms: tekenen %lu ms (versturen %lu), overig %lu ms",
                         (unsigned long)(dt / 1000), (unsigned long)(_refrUs / 1000),
                         (unsigned long)(_flushRoundUs / 1000), (unsigned long)((dt - _refrUs) / 1000));
            }
            if (dt > 1000) {
                _stats.frames++;
                _stats.handlerUs += dt;
                _stats.maxUs = std::max(_stats.maxUs, dt);
            }
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    M5GFX &gfx = *(M5GFX *)lv_display_get_driver_data(disp);
    int64_t flushStart = esp_timer_get_time();

    uint32_t w      = (area->x2 - area->x1 + 1);
    uint32_t h      = (area->y2 - area->y1 + 1);
    uint32_t pixels = w * h;

    // Zonder framebuffer-paneel gaat elke strook in een keer naar het paneel: een tweede schrijfopdracht
    // begint daar opnieuw bovenaan het venster (dat gaf een door elkaar geschoven beeld).
    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.writePixelsDMA((const lgfx::rgb565_t *)px_map, pixels);
    gfx.waitDMA();
    gfx.endWrite();

    _stats.flushUs += esp_timer_get_time() - flushStart;
    _flushRoundUs += (uint32_t)(esp_timer_get_time() - flushStart);
    _stats.pixels += (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    lv_display_flush_ready(disp);
}

static void lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    M5GFX &gfx = *(M5GFX *)lv_indev_get_driver_data(indev);

    auto tp = GetHAL().getTouchPoint();
    if (tp.num == 0) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = tp.x;
        data->point.y = tp.y;
    }
}

// Het AMOLED-paneel kan alleen gebieden schrijven die op een even pixel beginnen en op een oneven eindigen
static void rounder_cb(lv_event_t* e)
{
    lv_area_t* a = (lv_area_t*)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

void Hal::lvgl_init()
{
    mclog::tagInfo(_tag, "lvgl init");

    lv_init();

    static lv_display_t *disp = lv_display_create(_display->width(), _display->height());
    if (disp == NULL) {
        printf("lv_display_create failed\n");
        return;
    }

    lv_display_set_driver_data(disp, _display.get());
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, nullptr);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);

    // Kantoor: tekenbuffers bij voorkeur in snel intern RAM (28 regels RGB565), anders PSRAM
    size_t buf_size = _display->width() * 28 * 2;  // 28 regels: intern RAM is krap
    static uint8_t *buf1 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    static uint8_t *buf2 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        mclog::tagWarn(_tag, "no internal ram for draw buffers, using psram");
        free(buf1);
        free(buf2);
        buf_size = _display->width() * LV_BUFFER_LINE;
        buf1     = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        buf2     = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    }
    lv_display_set_buffers(disp, (void *)buf1, (void *)buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvTouchpad = lv_indev_create();
    LV_ASSERT_MALLOC(lvTouchpad);
    if (lvTouchpad == NULL) {
        printf("lv_indev_create failed\n");
        return;
    }
    lv_indev_set_driver_data(lvTouchpad, _display.get());
    lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchpad, lvgl_read_cb);
    lv_indev_set_display(lvTouchpad, disp);

    xGuiSemaphore                                     = xSemaphoreCreateMutex();
    const esp_timer_create_args_t periodic_timer_args = {.callback = &lvgl_tick_timer, .name = "lvgl_tick_timer"};
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 10 * 1000));
    // Kantoor: eigen kern en hogere prioriteit, zodat wifi en hoesverwerking het tekenen niet ophouden
    xTaskCreatePinnedToCore(lvgl_rtos_task, "lvgl_rtos_task", 10 * 1024, NULL, 4, NULL, 1);

    startLvglUpdate();

    {
        LvglLockGuard lock;
        uitk::lvgl_cpp::ScreenActive screen;
        screen.setBgColor(lv_color_black());
        GetHAL().bootLogo = std::make_unique<BootLogo>();
    }
}

bool Hal::lvglLock()
{
    return xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE ? true : false;
}

void Hal::lvglUnlock()
{
    xSemaphoreGive(xGuiSemaphore);
}

void Hal::startLvglUpdate()
{
    _lvgl_update_enabled = true;
}

void Hal::stopLvglUpdate()
{
    _lvgl_update_enabled = false;
}

std::string Hal::getDrawStats()
{
    LvglLockGuard lock;
    int64_t now    = esp_timer_get_time();
    double seconds = (now - _stats.sinceUs) / 1e6;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "periode %.1f s\nframes %lu (%.1f/s)\ngemiddeld %.1f ms, max %.1f ms\n"
             "waarvan versturen %.1f ms gemiddeld\npixels per frame %llu\n"
             "vrij intern %u KB, grootste blok %u KB\n",
             seconds, (unsigned long)_stats.frames, _stats.frames / (seconds > 0 ? seconds : 1),
             _stats.frames ? _stats.handlerUs / 1000.0 / _stats.frames : 0.0, _stats.maxUs / 1000.0,
             _stats.frames ? _stats.flushUs / 1000.0 / _stats.frames : 0.0,
             (unsigned long long)(_stats.frames ? _stats.pixels / _stats.frames : 0),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    _stats         = {};
    _stats.sinceUs = now;
    std::string out = buf;
    const auto& imu = GetHAL().getImuData();
    snprintf(buf, sizeof(buf), "versnelling (g) x %.2f  y %.2f  z %.2f\n", (double)imu.accelX, (double)imu.accelY,
             (double)imu.accelZ);
    return out + buf;
}

void Hal::prepareDeepSleep()
{
    mclog::tagInfo(_tag, "prepare deep sleep");
    stopLvglUpdate();
    stopVibrate();
    ioe_speaker_enable(false);
    if (_display) {
        _display->setBrightness(0);
        _display->sleep();  // AMOLED in slaapstand; blijft zo tijdens deep sleep
    }
    if (_cst820) {
        _cst820->sleep();
    }
}

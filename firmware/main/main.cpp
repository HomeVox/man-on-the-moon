/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <apps/common/audio/audio.h>
#include <esp_sleep.h>
#include <nvs_flash.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Man on the moon direct openen (geen launcher-menu)
    int jarvis = GetMooncake().installApp(std::make_unique<AppJarvis>());
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED && GetHAL().pmicWakeSource() == 0) {
        GetHAL().playBootSfx();  // alleen bij echt aanzetten, niet na deep sleep
    }
    GetMooncake().openApp(jarvis);

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetMooncake().update();
    }
}

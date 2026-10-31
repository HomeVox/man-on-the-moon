/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <bmi270_bmm150.h>
#include <bmi270.h>

#ifndef BMI2_I2C_PRIM_ADDR
#define BMI2_I2C_PRIM_ADDR 0x68
#endif

static const std::string_view _tag        = "HAL-IMU";
static bmi270_bmm150_handle_t _imu_sensor = NULL;

void Hal::imu_init()
{
    mclog::tagInfo(_tag, "init");

    bmi270_bmm150_config_t sensor_conf = {
        .i2c_addr        = BMI2_I2C_PRIM_ADDR,
        .config_file_ptr = NULL,
        // .mode            = BOSCH_ACCEL_AND_MAGN,
        .mode = BOSCH_ACCELEROMETER_ONLY,
    };

    if (bmi270_bmm150_sensor_create(_i2c_bus, &_imu_sensor, &sensor_conf) != ESP_OK) {
        mclog::tagError(_tag, "init failed");
        _imu_sensor = NULL;
    }
}

// Kantoor: "any-motion" van de BMI270 op beide interruptpinnen (open drain, actief laag),
// zodat de stroomchip ons bij beweging weer aanzet
bool Hal::imuEnableMotionInterrupt()
{
    if (_imu_sensor == NULL) {
        return false;
    }
    struct bmi2_dev* dev = &_imu_sensor->bmi2;

    struct bmi2_sens_config cfg = {};
    cfg.type                    = BMI2_ANY_MOTION;
    int8_t rslt                 = bmi270_get_sensor_config(&cfg, 1, dev);
    cfg.cfg.any_motion.duration  = 4;    // 4 x 20 ms aanhoudende beweging
    cfg.cfg.any_motion.threshold = 170;  // ~83 mg verandering
    cfg.cfg.any_motion.select_x  = BMI2_ENABLE;
    cfg.cfg.any_motion.select_y  = BMI2_ENABLE;
    cfg.cfg.any_motion.select_z  = BMI2_ENABLE;
    rslt |= bmi270_set_sensor_config(&cfg, 1, dev);

    uint8_t sensors[] = {BMI2_ACCEL, BMI2_ANY_MOTION};
    rslt |= bmi270_sensor_enable(sensors, 2, dev);

    struct bmi2_int_pin_config pin = {};
    rslt |= bmi2_get_int_pin_config(&pin, dev);
    pin.pin_type  = BMI2_INT_BOTH;
    pin.int_latch = BMI2_INT_NON_LATCH;
    for (auto& p : pin.pin_cfg) {
        p.lvl       = BMI2_INT_ACTIVE_LOW;
        p.od        = BMI2_INT_OPEN_DRAIN;
        p.output_en = BMI2_INT_OUTPUT_ENABLE;
        p.input_en  = BMI2_INT_INPUT_DISABLE;
    }
    rslt |= bmi2_set_int_pin_config(&pin, dev);

    struct bmi2_sens_int_config map = {};
    map.type       = BMI2_ANY_MOTION;
    map.hw_int_pin = BMI2_INT_BOTH;
    rslt |= bmi270_map_feat_int(&map, 1, dev);

    mclog::tagInfo(_tag, "motion interrupt: {}", rslt == BMI2_OK ? "ok" : "failed");
    return rslt == BMI2_OK;
}

void Hal::updateImuData()
{
    if (_imu_sensor != NULL) {
        int available = 0;
        if (bmi270_bmm150_sensor_acceleration_available(_imu_sensor, &available) == ESP_OK && available > 0) {
            bmi270_bmm150_sensor_read_acceleration(_imu_sensor, &_imu_data.accelY, &_imu_data.accelX,
                                                   &_imu_data.accelZ);
        }
        if (bmi270_bmm150_sensor_gyroscope_available(_imu_sensor, &available) == ESP_OK && available > 0) {
            bmi270_bmm150_sensor_read_gyroscope(_imu_sensor, &_imu_data.gyroY, &_imu_data.gyroX, &_imu_data.gyroZ);
        }
    } else {
        mclog::tagError(_tag, "imu invalid");
    }
}

/*
 * Copyright (c) 2020 Particle Industries, Inc.
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

#include "dct.h"

#include "tracker.h"
#include "tracker_cellular.h"
#include "mcp_can.h"

// Defines and constants
constexpr int CAN_SLEEP_RETRIES = 10; // Based on a series of 10ms delays

void ctrl_request_custom_handler(ctrl_request* req)
{
    auto result = SYSTEM_ERROR_NOT_SUPPORTED;
    if (Tracker::instance().isUsbCommandEnabled())
    {
        String command(req->request_data, req->request_size);
        if (CloudService::instance().dispatchCommand(command))
        {
            result = SYSTEM_ERROR_NONE;
        }
        else
        {
            result = SYSTEM_ERROR_INVALID_ARGUMENT;
        }
    }

    system_ctrl_set_result(req, result, nullptr, nullptr, nullptr);
}

Tracker *Tracker::_instance = nullptr;

Tracker::Tracker() :
    cloudService(CloudService::instance()),
    configService(ConfigService::instance()),
    locationService(LocationService::instance()),
    motionService(MotionService::instance()),
    rtc(AM1805_PIN_INVALID, RTC_AM1805_I2C_INSTANCE, RTC_AM1805_I2C_ADDR),
    location(TrackerLocation::instance()),
    motion(TrackerMotion::instance()),
    shipping(),
    rgb(TrackerRGB::instance())
{
    _config =
    {
        .UsbCommandEnable = true,
    };
}

int Tracker::registerConfig()
{
    static ConfigObject tracker_config("tracker", {
        ConfigBool("usb_cmd", &_config.UsbCommandEnable),
    });
    ConfigService::instance().registerModule(tracker_config);

    return 0;
}

void Tracker::initIo()
{
    // Initialize basic Tracker GPIO to known inactive values until they are needed later

    // ESP32 related GPIO
    pinMode(ESP32_BOOT_MODE_PIN, OUTPUT);
    digitalWrite(ESP32_BOOT_MODE_PIN, HIGH);
    pinMode(ESP32_PWR_EN_PIN, OUTPUT);
    digitalWrite(ESP32_PWR_EN_PIN, LOW); // power off device
    pinMode(ESP32_CS_PIN, OUTPUT);
    digitalWrite(ESP32_CS_PIN, HIGH);

    // CAN related GPIO
    pinMode(MCP_CAN_STBY_PIN, OUTPUT);
    digitalWrite(MCP_CAN_STBY_PIN, LOW);
    pinMode(MCP_CAN_PWR_EN_PIN, OUTPUT);
    digitalWrite(MCP_CAN_PWR_EN_PIN, HIGH); // The CAN 5V power supply will be enabled for a short period
    pinMode(MCP_CAN_RESETN_PIN, OUTPUT);
    digitalWrite(MCP_CAN_RESETN_PIN, HIGH);
    pinMode(MCP_CAN_INT_PIN, INPUT_PULLUP);
    pinMode(MCP_CAN_CS_PIN, OUTPUT);
    digitalWrite(MCP_CAN_CS_PIN, HIGH);

    // Reset CAN transceiver
    digitalWrite(MCP_CAN_RESETN_PIN, LOW);
    delay(50);
    digitalWrite(MCP_CAN_RESETN_PIN, HIGH);
    delay(50);

    // Initialize CAN device driver
    MCP_CAN can(MCP_CAN_CS_PIN, MCP_CAN_SPI_INTERFACE);
    if (can.begin(MCP_RX_ANY, CAN_1000KBPS, MCP_20MHZ) != CAN_OK)
    {
        Log.error("CAN init failed");
    }
    Log.info("CAN status: 0x%x", can.getCANStatus());
    if (can.setMode(MODE_NORMAL)) {
        Log.error("CAN mode to NORMAL failed");
    }
    else {
        Log.info("CAN mode to NORMAL");
    }
    delay(500);

    // Set to standby
    digitalWrite(MCP_CAN_STBY_PIN, HIGH);
    digitalWrite(MCP_CAN_PWR_EN_PIN, LOW); // The CAN 5V power supply will now be disabled

    for (int retries = CAN_SLEEP_RETRIES; retries >= 0; retries--) {
        auto stat = can.getCANStatus() & MODE_MASK;
        if (stat == MODE_SLEEP) {
            Log.info("CAN mode to SLEEP");
            break;
        }
        // Retry setting the sleep mode
        if (can.setMode(MODE_SLEEP)) {
            Log.error("CAN mode not set to SLEEP");
        }
        delay(10);
    }
}

void Tracker::enableWatchdog(bool enable) {
#ifndef RTC_WDT_DISABLE
    if (enable) {
        // watchdog at 1 minute
        rtc.configure_wdt(true, 15, AM1805_WDT_REGISTER_WRB_QUARTER_HZ);
        rtc.reset_wdt();
    }
    else {
        rtc.disable_wdt();
    }
#else
    (void)enable;
#endif // RTC_WDT_DISABLE
}

void Tracker::onSleepPrepare(TrackerSleepContext context)
{
    ConfigService::instance().flush();
}

void Tracker::onSleep(TrackerSleepContext context)
{
    if (_model == TRACKER_MODEL_TRACKERONE) {
        GnssLedEnable(false);
    }
}

void Tracker::onWake(TrackerSleepContext context)
{
    if (_model == TRACKER_MODEL_TRACKERONE) {
        GnssLedEnable(true);
    }
}

void Tracker::init()
{
    int ret = 0;

    last_loop_sec = System.uptime();

    // mark setup as complete to skip mobile app commissioning flow
    uint8_t val = 0;
    if(!dct_read_app_data_copy(DCT_SETUP_DONE_OFFSET, &val, DCT_SETUP_DONE_SIZE) && val != 1)
    {
        val = 1;
        dct_write_app_data(&val, DCT_SETUP_DONE_OFFSET, DCT_SETUP_DONE_SIZE);
    }

#ifndef TRACKER_MODEL_NUMBER
    ret = hal_get_device_hw_model(&_model, &_variant, nullptr);
    if (ret)
    {
        Log.error("Failed to read device model and variant");
    }
    else
    {
        Log.info("Tracker model = %04lX, variant = %04lX", _model, _variant);
    }
#else
    _model = TRACKER_MODEL_NUMBER;
#ifdef TRACKER_MODEL_VARIANT
    _variant = TRACKER_MODEL_VARIANT;
#else
    _variant = 0;
#endif // TRACKER_MODEL_VARIANT
#endif // TRACKER_MODEL_NUMBER

    // Initialize unused interfaces and pins
    initIo();

    CloudService::instance().init();

    ConfigService::instance().init();

    TrackerSleep::instance().init([this](bool enable){ this->enableWatchdog(enable); });
    TrackerSleep::instance().registerSleepPrepare([this](TrackerSleepContext context){ this->onSleepPrepare(context); });
    TrackerSleep::instance().registerSleep([this](TrackerSleepContext context){ this->onSleep(context); });
    TrackerSleep::instance().registerWake([this](TrackerSleepContext context){ this->onWake(context); });


    // Register our own configuration settings
    registerConfig();

    ret = LocationService::instance().begin(UBLOX_SPI_INTERFACE,
        UBLOX_CS_PIN,
        UBLOX_PWR_EN_PIN,
        UBLOX_TX_READY_MCU_PIN,
        UBLOX_TX_READY_GPS_PIN);
    if (ret)
    {
        Log.error("Failed to begin location service");
    }

    // Check for Tracker One hardware
    if (_model == TRACKER_MODEL_TRACKERONE)
    {
        (void)GnssLedInit();
        temperature_init(TRACKER_THERMISTOR);
    }

    MotionService::instance().start();

    location.init();

    motion.init();

    shipping.init();
    shipping.regShutdownCallback(std::bind(&Tracker::stop, this));

    rgb.init();

    rtc.begin();
    enableWatchdog(true);

    location.regLocGenCallback(loc_gen_cb);
}

void Tracker::loop()
{
    uint32_t cur_sec = System.uptime();

    // slow operations for once a second
    if(last_loop_sec != cur_sec)
    {
        last_loop_sec = cur_sec;

#ifndef RTC_WDT_DISABLE
        rtc.reset_wdt();
#endif
    }

    // fast operations for every loop
    TrackerSleep::instance().loop();
    TrackerMotion::instance().loop();

    // Check for Tracker One hardware
    if (_model == TRACKER_MODEL_TRACKERONE)
    {
        temperature_tick();

        if (temperature_high_events())
        {
            location.triggerLocPub(Trigger::NORMAL,"temp_h");
        }

        if (temperature_low_events())
        {
            location.triggerLocPub(Trigger::NORMAL,"temp_l");
        }
    }


    // fast operations for every loop
    CloudService::instance().tick();
    ConfigService::instance().tick();
    location.loop();
}

int Tracker::stop()
{
    LocationService::instance().stop();
    MotionService::instance().stop();

    return 0;
}

void Tracker::loc_gen_cb(JSONWriter& writer, LocationPoint &loc, const void *context)
{

    if(TrackerLocation::instance().getMinPublish())
    {
        // only add additional fields when not on minimal publish
        return;
    }

    // add cellular signal strength if available
    CellularSignal signal;
    if(!TrackerCellular::instance().getSignal(signal))
    {
        writer.name("cell").value(signal.getStrength(), 1);
    }

    // add lipo battery charge if available
    int bat_state = System.batteryState();
    if(bat_state == BATTERY_STATE_NOT_CHARGING ||
        bat_state == BATTERY_STATE_CHARGING ||
        bat_state == BATTERY_STATE_DISCHARGING ||
        bat_state == BATTERY_STATE_CHARGED)
    {
        float bat = System.batteryCharge();
        if(bat >= 0 && bat <= 100)
        {
            writer.name("batt").value(bat, 1);
        }
    }

    // Check for Tracker One hardware
    if (Tracker::instance().getModel() == TRACKER_MODEL_TRACKERONE)
    {
        writer.name("temp").value(get_temperature(), 1);
    }
}

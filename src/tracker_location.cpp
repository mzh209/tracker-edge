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

#include <stdint.h>

#include "Particle.h"
#include "tracker_config.h"
#include "tracker_location.h"

#include "config_service.h"
#include "location_service.h"

TrackerLocation *TrackerLocation::_instance = nullptr;

static constexpr system_tick_t sample_rate = 1000; // milliseconds

static int set_radius_cb(double value, const void *context)
{
    static_cast<LocationService *>((void *)context)->setRadiusThreshold(value);

    return 0;
}

static int get_radius_cb(double &value, const void *context)
{
    float temp;

    static_cast<LocationService *>((void *)context)->getRadiusThreshold(temp);

    value = temp;

    return 0;
}

// when entering the location config object copy from the actual config to the
// shadow for access if writing
int TrackerLocation::enter_location_config_cb(bool write, const void *context)
{
    if(write)
    {
        memcpy(&config_state_shadow, &config_state, sizeof(config_state_shadow));
    }
    return 0;
}

// when exiting the location config object copy from the shadow config to the
// actual if writing (and no error)
int TrackerLocation::exit_location_config_cb(bool write, int status, const void *context)
{
    if(write && !status)
    {
        if(config_state_shadow.interval_min_seconds > config_state_shadow.interval_max_seconds)
        {
            return -EINVAL;
        }
        memcpy(&config_state, &config_state_shadow, sizeof(config_state));
    }
    return status;
}

int TrackerLocation::get_loc_cb(CloudServiceStatus status,
    JSONValue *root,
    const void *context)
{
   triggerLocPub(Trigger::IMMEDIATE);
   return 0;
}

void TrackerLocation::init()
{
    static ConfigObject location_desc
    (
        "location",
        {
            ConfigFloat("radius", get_radius_cb, set_radius_cb, &LocationService::instance()).min(0.0).max(1000000.0),
            ConfigInt("interval_min", config_get_int32_cb, config_set_int32_cb,
                &config_state.interval_min_seconds, &config_state_shadow.interval_min_seconds,
                0, 86400l),
            ConfigInt("interval_max", config_get_int32_cb, config_set_int32_cb,
                &config_state.interval_max_seconds, &config_state_shadow.interval_max_seconds,
                0, 86400l),
            ConfigBool("min_publish",
                config_get_bool_cb, config_set_bool_cb,
                &config_state.min_publish, &config_state_shadow.min_publish),
            ConfigBool("lock_trigger",
                config_get_bool_cb, config_set_bool_cb,
                &config_state.lock_trigger, &config_state_shadow.lock_trigger)
        },
        std::bind(&TrackerLocation::enter_location_config_cb, this, _1, _2),
        std::bind(&TrackerLocation::exit_location_config_cb, this, _1, _2, _3)
    );

    ConfigService::instance().registerModule(location_desc);

    CloudService::instance().regCommandCallback("get_loc", &TrackerLocation::get_loc_cb, this);

    _last_location_publish_sec = System.uptime() - config_state.interval_min_seconds;

    TrackerSleep::instance().registerSleepPrepare([this](TrackerSleepContext context){ this->onSleepPrepare(context); });
    TrackerSleep::instance().registerSleep([this](TrackerSleepContext context){ this->onSleep(context); });
    TrackerSleep::instance().registerSleepCancel([this](TrackerSleepContext context){ this->onSleepCancel(context); });
    TrackerSleep::instance().registerWake([this](TrackerSleepContext context){ this->onWake(context); });
    TrackerSleep::instance().registerStateChange([this](TrackerSleepContext context){ this->onSleepState(context); });
}

int TrackerLocation::regLocGenCallback(
    std::function<void(JSONWriter&, LocationPoint &, const void *)> cb,
    const void *context)
{
    locGenCallbacks.append(std::bind(cb, _1, _2, context));
    return 0;
}

// register for callback on location publish success/fail
// these callbacks are NOT persistent and are used for the next publish
int TrackerLocation::regLocPubCallback(
    cloud_service_send_cb_t cb,
    const void *context)
{
    locPubCallbacks.append(std::bind(cb, _1, _2, _3, context));
    return 0;
}

int TrackerLocation::triggerLocPub(Trigger type, const char *s)
{
    std::lock_guard<std::recursive_mutex> lg(mutex);
    bool matched = false;

    for(auto trigger : _pending_triggers)
    {
        if(!strcmp(trigger, s))
        {
            matched = true;
            break;
        }
    }

    if(!matched)
    {
        _pending_triggers.append(s);
    }

    if(type == Trigger::IMMEDIATE)
    {
        _pending_immediate = true;
    }

    return 0;
}

void TrackerLocation::issue_location_publish_callbacks(CloudServiceStatus status, JSONValue *rsp_root, const char *req_event)
{
    for(auto cb : pendingLocPubCallbacks)
    {
        cb(status, rsp_root, req_event);
    }
    pendingLocPubCallbacks.clear();
}

int TrackerLocation::location_publish_cb(CloudServiceStatus status, JSONValue *rsp_root, const char *req_event, const void *context)
{
    bool issue_callbacks = true;

    if(status == CloudServiceStatus::SUCCESS)
    {
        // this could either be on the Particle Cloud ack (default) OR the
        // end-to-end ACK
        Log.info("location cb publish %lu success!", *(uint32_t *) context);
        _first_publish = false;
    }
    else if(status == CloudServiceStatus::FAILURE)
    {
        // right now FAILURE only comes out of a Particle Cloud issue
        // once Particle Cloud passes if waiting on end-to-end it will
        // only ever timeout

        // save on failure for retry
        if(req_event && !location_publish_retry_str)
        {
            size_t len = strlen(req_event) + 1;
            location_publish_retry_str = (char *) malloc(len);
            if(location_publish_retry_str)
            {
                memcpy(location_publish_retry_str, req_event, len);
                // we've saved for retry, defer callbacks until retry completes
                issue_callbacks = false;
            }
        }
        Log.info("location cb publish %lu failure", *(uint32_t *) context);
    }
    else if(status == CloudServiceStatus::TIMEOUT)
    {
        Log.info("location cb publish %lu timeout", *(uint32_t *) context);
    }
    else
    {
        Log.info("location cb publish %lu unexpected status: %d", *(uint32_t *) context, status);
    }

    if(issue_callbacks)
    {
        issue_location_publish_callbacks(status, rsp_root, req_event);
    }

    return 0;
}

void TrackerLocation::location_publish()
{
    int rval;
    CloudService &cloud_service = CloudService::instance();

    // maintain cloud service lock across the send to allow us to save off
    // the finalized loc publish to retry on failure
    cloud_service.lock();

    if(location_publish_retry_str)
    {
        // publish a retry loc
        rval = cloud_service.send(location_publish_retry_str,
            WITH_ACK,
            CloudServicePublishFlags::FULL_ACK,
            &TrackerLocation::location_publish_cb, this,
            CLOUD_DEFAULT_TIMEOUT_MS, &_last_location_publish_sec);
    }
    else
    {
        // publish a new loc (contained in cloud_service buffer)
        rval = cloud_service.send(WITH_ACK,
            CloudServicePublishFlags::FULL_ACK,
            &TrackerLocation::location_publish_cb, this,
            CLOUD_DEFAULT_TIMEOUT_MS, &_last_location_publish_sec);
    }

    if(rval == -EBUSY)
    {
        // this implies a transient failure that should recover very
        // quickly (normally another publish in progress blocking lower
        // in the system)
        // save off the generated publish to retry as it has already
        // consumed pending events if applicable
        if(!location_publish_retry_str)
        {
            size_t len = strlen(cloud_service.writer().buffer()) + 1;
            location_publish_retry_str = (char *) malloc(len);
            if(location_publish_retry_str)
            {
                memcpy(location_publish_retry_str, cloud_service.writer().buffer(), len);
            }
            else
            {
                // generated successfuly but unable to save off a copy to retry
                issue_location_publish_callbacks(CloudServiceStatus::FAILURE, NULL, cloud_service.writer().buffer());
            }
        }
    }
    else
    {
        if(rval)
        {
            issue_location_publish_callbacks(CloudServiceStatus::FAILURE, NULL, location_publish_retry_str);
        }
        if(location_publish_retry_str)
        {
            // on success or fatal failure free it
            free(location_publish_retry_str);
            location_publish_retry_str = nullptr;
        }
    }
    cloud_service.unlock();
}

void TrackerLocation::enableNetwork() {
    LocationService::instance().start();
    TrackerSleep::instance().forceFullWakeCycle();
}

void TrackerLocation::disableNetwork() {
    LocationService::instance().stop();
}

EvaluationResults TrackerLocation::evaluatePublish(uint32_t allowance) {
    if (_pending_immediate) {
        // request for immediate publish overrides the default min/max interval checking
        Log.trace("%s pending_immediate", __FUNCTION__);
        return EvaluationResults {PublishReason::IMMEDIATE, true};
    }

    uint32_t interval = System.uptime() - _last_location_publish_sec;

    uint32_t max = ((uint32_t)config_state.interval_max_seconds > allowance) ?
        ((uint32_t)config_state.interval_max_seconds - allowance) : (uint32_t)config_state.interval_max_seconds;

    if (config_state.interval_max_seconds &&
        (interval >= max)) {
        // max interval and past the max interval so have to publish
        Log.trace("%s max", __FUNCTION__);
        return EvaluationResults {PublishReason::TIME, true};
    }

    uint32_t min = ((uint32_t)config_state.interval_min_seconds > allowance) ?
        ((uint32_t)config_state.interval_min_seconds - allowance) : (uint32_t)config_state.interval_min_seconds;

    if (_pending_triggers.size() &&
        (!config_state.interval_min_seconds ||
        (interval >= min))) {
        // no min interval or past the min interval so can publish
        Log.trace("%s min", __FUNCTION__);
        return EvaluationResults {PublishReason::TRIGGERS, true};
    }

    // This will allow a trigger publish on boot
    if (_first_publish) {
        Log.trace("%s first", __FUNCTION__);
        return EvaluationResults {PublishReason::TRIGGERS, true};
    }

    return EvaluationResults {PublishReason::NONE, false};
}

void TrackerLocation::onSleepPrepare(TrackerSleepContext context) {
    unsigned int wake = _last_location_publish_sec;
    int32_t interval = 0;
    if (_pending_triggers.size()) {
        interval = config_state.interval_min_seconds;
        wake += interval;
    }
    else {
        interval = config_state.interval_max_seconds;
        wake += interval;
    }
    TrackerSleep::instance().wakeAtSeconds(wake);
    Log.trace("%s last=%lu, interval=%ld", __FUNCTION__, _last_location_publish_sec, interval);
}

bool TrackerLocation::isSleepEnabled() {
    return (TrackerSleep::instance().getConfigMode() != TrackerSleepMode::Disable);
}

void TrackerLocation::onSleep(TrackerSleepContext context) {
    disableNetwork();
}

void TrackerLocation::onSleepCancel(TrackerSleepContext context) {

}

void TrackerLocation::onWake(TrackerSleepContext context) {
    auto result = evaluatePublish(
        std::min(TrackerSleep::instance().getConfigConnectingTime(), config_state.interval_min_seconds)
        );

    if (result.networkNeeded) {
        enableNetwork();
        Log.trace("%s needs to start the network", __FUNCTION__);
    }
}

void TrackerLocation::onSleepState(TrackerSleepContext context) {
    if (context.reason == TrackerSleepReason::STATE_TO_CONNECTING) {
        Log.trace("%s starting GNSS", __FUNCTION__);
        LocationService::instance().start();
    }
}

GnssState TrackerLocation::loopLocation(LocationPoint& cur_loc) {
    static GnssState lastGnssState = GnssState::OFF;
    GnssState currentGnssState = GnssState::ON_LOCKED_STABLE;

    LocationStatus locStatus;
    LocationService::instance().getStatus(locStatus);

    do {
        if (!locStatus.powered) {
            currentGnssState = GnssState::OFF;
            break;
        }

        if (LocationService::instance().getLocation(cur_loc) != SYSTEM_ERROR_NONE)
        {
            currentGnssState = GnssState::ERROR;
            break;
        }

        if (!cur_loc.locked) {
            currentGnssState = GnssState::ON_UNLOCKED;
            break;
        }

        if (!cur_loc.stable) {
            currentGnssState = GnssState::ON_LOCKED_UNSTABLE;
            break;
        }

        float radius;
        LocationService::instance().getRadiusThreshold(radius);
        if (radius) {
            bool outside;
            LocationService::instance().isOutsideRadius(outside, cur_loc);
            if (outside) {
                triggerLocPub(Trigger::NORMAL,"radius");
            }
        }
    } while (false);

    // Detect GNSS locked changes
    if (config_state.lock_trigger &&
        (currentGnssState == GnssState::ON_LOCKED_STABLE) &&
        (currentGnssState != lastGnssState)) {

        triggerLocPub(Trigger::NORMAL,"lock");
    }

    lastGnssState = currentGnssState;

    return currentGnssState;
}

void TrackerLocation::buildPublish(LocationPoint& cur_loc) {
    if(cur_loc.locked)
    {
        LocationService::instance().setWayPoint(cur_loc.latitude, cur_loc.longitude);
    }

    CloudService &cloud_service = CloudService::instance();
    cloud_service.beginCommand("loc");
    cloud_service.writer().name("loc").beginObject();
    if (cur_loc.locked)
    {
        cloud_service.writer().name("lck").value(1);
        cloud_service.writer().name("time").value((unsigned int) cur_loc.epochTime);
        cloud_service.writer().name("lat").value(cur_loc.latitude, 8);
        cloud_service.writer().name("lon").value(cur_loc.longitude, 8);
        if(!config_state.min_publish)
        {
            cloud_service.writer().name("alt").value(cur_loc.altitude, 3);
            cloud_service.writer().name("hd").value(cur_loc.heading, 2);
            cloud_service.writer().name("h_acc").value(cur_loc.horizontalAccuracy, 3);
            cloud_service.writer().name("v_acc").value(cur_loc.verticalAccuracy, 3);
        }
    }
    else
    {
        cloud_service.writer().name("lck").value(0);
    }
    for(auto cb : locGenCallbacks)
    {
        cb(cloud_service.writer(), cur_loc);
    }
    cloud_service.writer().endObject();
    if(!_pending_triggers.isEmpty())
    {
        std::lock_guard<std::recursive_mutex> lg(mutex);
        cloud_service.writer().name("trig").beginArray();
        for (auto trigger : _pending_triggers) {
            cloud_service.writer().value(trigger);
        }
        _pending_triggers.clear();
        cloud_service.writer().endArray();
    }
    Log.info("%.*s", cloud_service.writer().dataSize(), cloud_service.writer().buffer());
}

void TrackerLocation::loop() {
    static system_tick_t last_sample = 0;

    // The rest of this loop should only sample as fast as necessary
    if (millis() - last_sample < sample_rate)
    {
        return;
    }
    last_sample = millis();

    // First take care of any retry attempts of last loc
    if (location_publish_retry_str && Particle.connected())
    {
        Log.info("retry failed publish");
        location_publish();
    }

    // Gather current location information and status
    LocationPoint cur_loc;
    auto locationStatus = loopLocation(cur_loc);

    // TODO!!! Delete this
    switch (locationStatus) {
        case GnssState::OFF: {
            Log.trace("GNSS off");
            break;
        }

        case GnssState::ON_LOCKED_STABLE: {
            Log.trace("GNSS stable");
            break;
        }

        case GnssState::ON_UNLOCKED: {
            Log.trace("GNSS unlocked");
            break;
        }

        case GnssState::ON_LOCKED_UNSTABLE: {
            Log.trace("GNSS stablizing");
            break;
        }
    }

    // Perform interval evaluation
    uint32_t allowance = 0;
    if (isSleepEnabled()) {
        allowance = std::min(TrackerSleep::instance().getConfigConnectingTime(), config_state.interval_min_seconds);
    }
    auto publishReason = evaluatePublish(allowance);

    // TODO!!! delete me
    if (TrackerSleep::instance().isFullWakeCycle()) {
        Log.trace("currently in full wake");
    }
    else {
        Log.trace("currently NOT in full wake");
    }

    // TODO!!! delete me
    if (publishReason.networkNeeded) {
        Log.trace("need the network");
    }
    else {
        Log.trace("don't need the network");
    }

    // This evaluation may have performed earlier and determined that no network was needed.  Check again
    // because this loop may overlap with required network operations.
    if (!TrackerSleep::instance().isFullWakeCycle() && publishReason.networkNeeded) {
        enableNetwork();
    }

    bool publishNow = false;

    //                                   : NONE      TIME        TRIG        IMM
    //                                    ----------------------------------------
    // GnssState::OFF                       NA       PUB         PUB         PUB
    // GnssState::ON_UNLOCKED               NA       WAIT        WAIT        PUB
    // GnssState::ON_LOCKED_UNSTABLE        NA       WAIT        WAIT        PUB
    // GnssState::ON_LOCKED_STABLE          NA       PUB         PUB         PUB

    switch (publishReason.reason) {
        case PublishReason::NONE: {
            // If there is nothing to do then get out
            return;
        }

        case PublishReason::TIME: {
            switch (locationStatus) {
                case GnssState::ON_LOCKED_STABLE: {
                    Log.trace("publishing from max interval");
                    triggerLocPub(Trigger::NORMAL,"time");
                    publishNow = true;
                    break;
                }

                case GnssState::OFF:
                // fall through
                case GnssState::ON_UNLOCKED:
                // fall through
                case GnssState::ON_LOCKED_UNSTABLE: {
                    Log.trace("waiting for stable GNSS lock for max interval");
                    break;
                }
            }
            break;
        }

        case PublishReason::TRIGGERS: {
            switch (locationStatus) {
                case GnssState::ON_LOCKED_STABLE: {
                    Log.trace("publishing from triggers");
                    publishNow = true;
                    break;
                }

                case GnssState::OFF:
                // fall through
                case GnssState::ON_UNLOCKED:
                // fall through
                case GnssState::ON_LOCKED_UNSTABLE: {
                    Log.trace("waiting for stable GNSS lock for triggers");
                    break;
                }
            }
            break;
        }

        case PublishReason::IMMEDIATE: {
            Log.trace("publishing from immediate");
            _pending_immediate = false;
            publishNow = true;
            break;
        }
    }

    //
    // Perform publish of location data if requested
    //

    // then of any new publish
    if(publishNow)
    {
        if(location_publish_retry_str)
        {
            Log.info("freeing unsuccessful retry");
            // retried attempt not completed in time for new publish
            // drop and issue callbacks
            issue_location_publish_callbacks(CloudServiceStatus::TIMEOUT, NULL, location_publish_retry_str);
            free(location_publish_retry_str);
            location_publish_retry_str = nullptr;
        }
        Log.info("publishing now...");
        _last_location_publish_sec = System.uptime();
        buildPublish(cur_loc);

        pendingLocPubCallbacks = locPubCallbacks;
        locPubCallbacks.clear();
        location_publish();
    }
}

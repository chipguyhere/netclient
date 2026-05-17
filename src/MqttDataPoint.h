// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef MQTT_DATAPOINT_H
#define MQTT_DATAPOINT_H

// ============================================================================
// MqttDataPoint: smart-publishing wrappers around an MQTT topic.
// ============================================================================
//
// A DataPoint represents one value (numeric or string) you want kept mirrored
// at an MQTT broker. The application updates the value as fast as it likes
// — even from a high-rate sensor task — and the DataPoint decides when to
// actually publish, balancing freshness against broker churn.
//
// Two flavors:
//
//   MqttNumericDataPoint
//     Publishes a float, formatted to a configured number of decimal places.
//     A "significance threshold" gates publication: small drifts in the
//     value are absorbed and carried out only by the periodic re-publish,
//     but a change that exceeds the threshold publishes promptly. Good for
//     slow-trending readings (temperature, voltage, RSSI) where you want
//     the dashboard to react to real movement but ignore jitter.
//
//   MqttStringDataPoint
//     Publishes free-form text up to 40 characters. Includes debouncing
//     and rate-limiting so a bursty signal source can't flood the broker:
//     a hard cap of 20 publishes per minute per DataPoint, separate
//     "rapid-change" detection (10+ changes in 10s), and a "final report
//     on stability" rule (5s) that guarantees the *last* value of a flurry
//     is delivered once things settle.
//
// Common features:
//
//   - Periodic re-publish of the current value (default 50s, configurable
//     per instance) so retained messages don't age out on brokers that
//     expire them.
//   - Per-update TTL: updateValue(value, expirationSeconds) marks the value
//     as expiring after that many seconds; once expired the DataPoint stops
//     re-publishing — that's "sensor unplugged" semantics, where a stale
//     reading is worse than no reading. updateNonExpiringValue() skips the
//     TTL for sticky readings (firmware version, MAC, etc.).
//   - requireReportNow() bypasses the timers and forces a publish on the
//     next tick.
//   - registerPublishCallback() lets external code observe a DataPoint's
//     publishes; MqttDataPointWatcher uses this to react to local value
//     changes without waiting for a broker round-trip.
//
// Topic formatting:
//
//   The constructor takes a printf-style format. A literal "%P" at the very
//   start of the format is preserved verbatim through formatTopicV();
//   NetClient.publish() then expands it to the configured topic prefix at
//   publish time. So
//       new MqttStringDataPoint("%P/sensor/temp")
//   reads exactly as you'd expect.
//
// ----------------------------------------------------------------------------
// Implementation notes — skip on first read.
//
//   Threading model:
//     - tick() and tickAll() are driven internally by NetClient's network
//       task. Applications should not call them; the "publishing thread" is
//       NetClient's task.
//     - tickAll() snapshots the instance list under a mutex before iterating,
//       so concurrent construction/destruction of DataPoints from other tasks
//       does not invalidate it.
//     - updateValue() and updateNonExpiringValue() are thread-safe and may
//       be called from any task once NetClient.begin() has returned.
//     - Constructors and destructors are thread-safe — DataPoints may be
//       created or destroyed from multiple tasks concurrently, including
//       while NetClient's task is iterating in tickAll().
//     - The setStable.../setChanging.../setDataExpiration... setters write
//       directly to instance fields without locking; treat them as setup-
//       time configuration on each instance.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <stdarg.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class MqttDataPoint;

// Callback type for publish notifications
typedef void (*PublishCallback)(const char* topic, const char* payload);

// Base class for all MQTT data points
class MqttDataPoint {
private:
    // Static list of all data point instances
    static std::vector<MqttDataPoint*> allInstances;

    // List of publish callbacks for this instance
    std::vector<PublishCallback> publishCallbacks;

protected:
    char* topic;                         // MQTT topic for this data point (dynamically allocated)
    unsigned long lastSuccessfulReport;  // millis() of last successful report
    unsigned long lastUnsuccessfulReport;// millis() of last unsuccessful report
    unsigned long lastSensorUpdate;      // millis() of last sensor reading
    unsigned long expiresAtMillis;       // millis() when data expires (0 = never expires)
    unsigned long dataExpirationSeconds; // Default seconds until data expires (default 120 seconds = 2 minutes)
    unsigned long stableReportIntervalSeconds;    // Report interval for stable values (default 50 seconds)
    unsigned long changingReportIntervalSeconds;  // Minimum interval for changing values (default 3 seconds)
    bool sensorValid;                    // Explicit sensor validity flag
    bool hasBeenReported;                // Track if initial report has been made
    bool forceReport;                    // Force immediate report on next tick

public:
    MqttDataPoint(const char* topicFormat, ...) __attribute__((format(printf, 2, 3)));
    virtual ~MqttDataPoint();

    // Called regularly from the main loop to check timers and perform MQTT
    // reporting via the global NetClient. Returns true if a publish was queued
    // to a connected broker during this tick.
    virtual bool tick() = 0;

    // Tick every registered MqttDataPoint instance. Returns true if any of them
    // queued a publish.
    static bool tickAll();

    // Force immediate report on next tick (bypasses normal timing logic)
    void requireReportNow();

    // Check if data has expired
    bool isDataExpired() const;

    // Get seconds since last sensor update
    unsigned long getSecondsSinceLastUpdate() const;

    // Get seconds until data expires (negative if already expired)
    long getSecondsToExpiration() const;

    // Get last successful/unsuccessful report times
    unsigned long getLastSuccessfulReport() const { return lastSuccessfulReport; }
    unsigned long getLastUnsuccessfulReport() const { return lastUnsuccessfulReport; }

    // Set data expiration time in seconds (default 120 seconds = 2 minutes)
    void setDataExpirationSeconds(unsigned long seconds) { dataExpirationSeconds = seconds; }

    // Set reporting intervals in seconds
    void setStableReportIntervalSeconds(unsigned long seconds) { stableReportIntervalSeconds = seconds; }
    void setChangingReportIntervalSeconds(unsigned long seconds) { changingReportIntervalSeconds = seconds; }

    // Static method to get all data point instances
    static const std::vector<MqttDataPoint*>& getAllInstances() { return allInstances; }

    // Register a callback to be notified when this data point publishes
    void registerPublishCallback(PublishCallback callback);

    // Get the topic for this data point
    const char* getTopic() const { return topic; }

    // Format-print like vsnprintf, with one extension: a literal "%P" at the
    // very start of the format string is copied verbatim to the output
    // instead of being parsed as a format specifier (which is undefined
    // behavior in standard printf). This lets topic formats like "%P/foo"
    // survive construction-time formatting; NetClient.publish() then expands
    // the surviving "%P" to the current topic prefix at publish time.
    //
    // Behavior matches vsnprintf otherwise — pass dst==nullptr / dstSize==0
    // for size-only computation, and call again with an allocated buffer to
    // produce the string. (Re-prepare the va_list between calls; vsnprintf
    // consumes it.)
    static int formatTopicV(char* dst, size_t dstSize,
                            const char* topicFormat, va_list args);

protected:
    // Protected default constructor for derived classes (topic will be set by derived class)
    MqttDataPoint();

    // Protected helper for derived classes to format and set topic
    void setFormattedTopic(const char* topicFormat, va_list args);

    // Attempt to publish via the global NetClient. Returns true if NetClient is
    // currently connected to MQTT and the publish was queued. retained defaults
    // to true.
    bool attemptPublish(const char* payload, bool retained = true);
};

// Numeric data point with significance-based reporting
class MqttNumericDataPoint : public MqttDataPoint {
private:
    // Static semaphore for all instances (controls access to all update buffers)
    static SemaphoreHandle_t updateSemaphore;
    static bool semaphoreInitialized;

    // Per-instance update buffer (written by updating thread)
    struct UpdateBuffer {
        float value;
        unsigned long expirationSeconds;
        bool isNonExpiring;
    };
    UpdateBuffer updateBuffer;
    volatile bool updatePending;           // Flag set by updating thread when new data is available

    // Current value (read by publishing thread)
    float currentValue;
    float lastReportedValue;
    float significanceThreshold;           // Threshold for considering change significant
    int decimalPlaces;                     // Number of decimal places to round to
    bool hasValue;                         // Track if we've received a value yet
    bool pendingReport;                    // Track if we have an unreported significant change

    // Helper to round value to specified decimal places
    float roundToDecimalPlaces(float value) const;

    // Sync from update buffer to current value (called by publishing thread)
    void syncFromUpdateBuffer();

public:
    MqttNumericDataPoint(int decimalPlaces, float significanceThreshold, const char* topicFormat, ...) __attribute__((format(printf, 4, 5)));

    // Update the sensor reading with expiration time in seconds.
    // Thread-safe: callable from any task once NetClient.begin() has returned.
    void updateValue(float value, unsigned long expirationSeconds);

    // Update with a non-expiring value (good until next update).
    // Thread-safe: callable from any task once NetClient.begin() has returned.
    void updateNonExpiringValue(float value);

    // Get current value
    float getValue() const { return currentValue; }

    // Check if we have a valid value
    bool hasValidValue() const { return hasValue && sensorValid && !isDataExpired(); }

    // Set significance threshold
    void setSignificanceThreshold(float threshold) { significanceThreshold = threshold; }

    // Override tick to implement numeric-specific logic
    virtual bool tick() override;

private:
    // Check if current value change is significant
    bool isChangeSignificant() const;

    // Determine if we need to report now
    bool needsReport() const;

    // Perform the actual report
    bool doReport();
};

// String data point with debouncing and rate limiting
class MqttStringDataPoint : public MqttDataPoint {
private:
    // Static semaphore for all instances (controls access to all update buffers)
    static SemaphoreHandle_t updateSemaphore;
    static bool semaphoreInitialized;

    // Per-instance update buffer (written by updating thread)
    struct UpdateBuffer {
        char value[41];        // 40 chars + null terminator
        unsigned long expirationSeconds;
        bool isNonExpiring;
    };
    UpdateBuffer updateBuffer;
    volatile bool updatePending;           // Flag set by updating thread when new data is available

    // Current value (read by publishing thread)
    char currentValue[41];     // 40 chars + null terminator
    char lastReportedValue[41];
    bool hasValue;
    bool pendingReport;        // Track if we have an unreported change
    unsigned long lastValueChange; // millis() when currentValue last changed

    // Debouncing and rate limiting
    static const int MAX_REPORTS_PER_WINDOW = 20;
    static const int REPORT_WINDOW_MS = 60000;  // 60 seconds
    unsigned long reportTimestamps[MAX_REPORTS_PER_WINDOW];
    int reportCount;
    int reportIndex;

    // Change detection
    unsigned long firstChangeTime;
    int changeCount;
    static const int RAPID_CHANGE_THRESHOLD = 10;  // Changes in 10 seconds
    static const unsigned long RAPID_CHANGE_WINDOW = 10000;  // 10 seconds
    static const unsigned long FINAL_REPORT_STABILITY_MS = 5000;  // 5 seconds of stability for final report

    // Sync from update buffer to current value (called by publishing thread)
    void syncFromUpdateBuffer();

public:
    MqttStringDataPoint(const char* topicFormat, ...) __attribute__((format(printf, 2, 3)));

    // Update the sensor reading with expiration time in seconds.
    // Thread-safe: callable from any task once NetClient.begin() has returned.
    void updateValue(const char* value, unsigned long expirationSeconds);

    // Update with a non-expiring value (good until next update).
    // Thread-safe: callable from any task once NetClient.begin() has returned.
    void updateNonExpiringValue(const char* value);

    // Get current value
    const char* getValue() const { return currentValue; }

    // Check if we have a valid value
    bool hasValidValue() const { return hasValue && sensorValid && !isDataExpired(); }

    // Override tick to implement string-specific logic
    virtual bool tick() override;

private:
    // Check if state has changed
    bool hasStateChanged() const;

    // Check if we're in rapid change mode
    bool isRapidlyChanging() const;

    // Check if we can report (within rate limit)
    bool canReport() const;

    // Record a report timestamp
    void recordReport();

    // Determine if we need to report now
    bool needsReport() const;

    // Perform the actual report
    bool doReport();
};

#endif // MQTT_DATAPOINT_H

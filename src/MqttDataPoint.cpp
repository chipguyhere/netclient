// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#include "MqttDataPoint.h"
#include "chipguy_MQTT_netclient.h"
#include <string.h>
#include <algorithm>

// ============================================================================
// MqttDataPoint (Base Class) Implementation
// ============================================================================

// Define static member
std::vector<MqttDataPoint*> MqttDataPoint::allInstances;

int MqttDataPoint::formatTopicV(char* dst, size_t dstSize,
                                const char* topicFormat, va_list args) {
    if (topicFormat && topicFormat[0] == '%' && topicFormat[1] == 'P') {
        char*  restDst  = nullptr;
        size_t restSize = 0;
        if (dst && dstSize >= 2) {
            dst[0] = '%';
            dst[1] = 'P';
            restDst  = dst + 2;
            restSize = dstSize - 2;
        }
        int restLen = vsnprintf(restDst, restSize, topicFormat + 2, args);
        return (restLen < 0) ? restLen : (2 + restLen);
    }
    return vsnprintf(dst, dstSize, topicFormat, args);
}

// ----------------------------------------------------------------------------
// Global construction / iteration mutex.
//
// Guards modifications to allInstances (constructor push_back, destructor
// erase) and the lazy creation of each subclass's static updateSemaphore, so
// that multiple tasks may construct DataPoints concurrently and the main
// loop's tickAll() may iterate while construction happens.
//
// The mutex itself is created lazily on first need. Bootstrap is protected by
// a portMUX_TYPE — that's zero-init via portMUX_INITIALIZER_UNLOCKED, so it
// has no bootstrap problem of its own. The actual xSemaphoreCreateMutex()
// call happens *outside* the portMUX critical section (it can malloc and
// touch heap mutexes, which would be unsafe with interrupts disabled); under
// the portMUX we only do an atomic null-check + pointer install, with a fall
// back to vSemaphoreDelete if a concurrent caller installed first.
// ----------------------------------------------------------------------------
namespace {
portMUX_TYPE       s_listBootstrapMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t  s_listMutex        = nullptr;

SemaphoreHandle_t getListMutex() {
    SemaphoreHandle_t cached = s_listMutex;
    if (cached) return cached;

    SemaphoreHandle_t fresh = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&s_listBootstrapMux);
    if (!s_listMutex) {
        s_listMutex = fresh;
        fresh = nullptr;
    }
    cached = s_listMutex;
    portEXIT_CRITICAL(&s_listBootstrapMux);
    if (fresh) vSemaphoreDelete(fresh);  // we lost the bootstrap race
    return cached;
}

// Tiny RAII helper so we don't forget to give the mutex back on every path.
struct ListLock {
    SemaphoreHandle_t mux;
    ListLock() : mux(getListMutex()) { xSemaphoreTake(mux, portMAX_DELAY); }
    ~ListLock()                       { xSemaphoreGive(mux); }
};
} // namespace

MqttDataPoint::MqttDataPoint(const char* topicFormat, ...)
    : lastSuccessfulReport(0),
      lastUnsuccessfulReport(0),
      lastSensorUpdate(0),
      expiresAtMillis(0),          // 0 = no expiration set yet
      dataExpirationSeconds(120),  // Default 120 seconds (2 minutes)
      stableReportIntervalSeconds(50),      // Default 50 seconds for stable values
      changingReportIntervalSeconds(3),     // Default 3 seconds for changing values
      sensorValid(true),
      hasBeenReported(false),
      forceReport(false)
{
    // Determine required buffer size
    va_list args;
    va_start(args, topicFormat);
    int len = formatTopicV(nullptr, 0, topicFormat, args);
    va_end(args);

    // Allocate exactly the right amount of memory
    topic = new char[len + 1];

    // Format the topic string
    va_start(args, topicFormat);
    formatTopicV(topic, len + 1, topicFormat, args);
    va_end(args);

    // Add this instance to the static list
    {
        ListLock lock;
        allInstances.push_back(this);
    }
}

MqttDataPoint::MqttDataPoint()
    : topic(nullptr),
      lastSuccessfulReport(0),
      lastUnsuccessfulReport(0),
      lastSensorUpdate(0),
      expiresAtMillis(0),          // 0 = no expiration set yet
      dataExpirationSeconds(120),  // Default 120 seconds (2 minutes)
      stableReportIntervalSeconds(50),      // Default 50 seconds for stable values
      changingReportIntervalSeconds(3),     // Default 3 seconds for changing values
      sensorValid(true),
      hasBeenReported(false),
      forceReport(false)
{
    // Add this instance to the static list
    {
        ListLock lock;
        allInstances.push_back(this);
    }
}

MqttDataPoint::~MqttDataPoint() {
    // Remove this instance from the static list
    {
        ListLock lock;
        auto it = std::find(allInstances.begin(), allInstances.end(), this);
        if (it != allInstances.end()) {
            allInstances.erase(it);
        }
    }

    // Free the dynamically allocated topic
    if (topic != nullptr) {
        delete[] topic;
    }
}

void MqttDataPoint::setFormattedTopic(const char* topicFormat, va_list args) {
    // Determine required buffer size
    va_list args_copy;
    va_copy(args_copy, args);
    int len = formatTopicV(nullptr, 0, topicFormat, args_copy);
    va_end(args_copy);

    // Allocate exactly the right amount of memory
    topic = new char[len + 1];

    // Format the topic string
    formatTopicV(topic, len + 1, topicFormat, args);
}

void MqttDataPoint::requireReportNow() {
    forceReport = true;
}

void MqttDataPoint::registerPublishCallback(PublishCallback callback) {
    publishCallbacks.push_back(callback);
}

bool MqttDataPoint::isDataExpired() const {
    if (expiresAtMillis == 0) {
        return false;  // Non-expiring or no update yet
    }
    long x = millis() - expiresAtMillis;
    return (x > 0);
}

unsigned long MqttDataPoint::getSecondsSinceLastUpdate() const {
    if (lastSensorUpdate == 0) {
        return 0;  // No update yet
    }
    long ageMsl = millis() - lastSensorUpdate;
    if (ageMsl < 0) ageMsl = 0;
    long ageMs = ageMsl;
    return ageMs / 1000UL;
}

long MqttDataPoint::getSecondsToExpiration() const {
    if (lastSensorUpdate == 0) {
        return (long)dataExpirationSeconds;  // Full expiration time available
    }
    unsigned long secondsSinceUpdate = getSecondsSinceLastUpdate();
    return (long)dataExpirationSeconds - (long)secondsSinceUpdate;
}

bool MqttDataPoint::attemptPublish(const char* payload, bool retained) {
    // Gate on broker A connectivity. NetClient.publish() always returns true
    // (it just queues), so we have to consult isMqttConnected() ourselves to
    // decide whether to count this as a successful or unsuccessful report.
    if (!NetClient.isMqttConnected()) {
        lastUnsuccessfulReport = millis();
        return false;
    }

    bool success = NetClient.publish(topic, payload, retained);

    if (success) {
        lastSuccessfulReport = millis();
        hasBeenReported = true;

        // Notify all registered callbacks
        for (auto callback : publishCallbacks) {
            if (callback) {
                callback(topic, payload);
            }
        }
    } else {
        lastUnsuccessfulReport = millis();
    }

    return success;
}

bool MqttDataPoint::tickAll() {
    // Snapshot the list under the mutex (cheap pointer copy), then iterate
    // outside it. This keeps tick() free to do publishes / take other locks
    // without holding ours, and lets construction from other tasks proceed
    // without being serialized against the iteration.
    std::vector<MqttDataPoint*> snapshot;
    {
        ListLock lock;
        snapshot = allInstances;
    }
    bool anyPublished = false;
    for (auto* instance : snapshot) {
        if (instance && instance->tick()) {
            anyPublished = true;
        }
    }
    return anyPublished;
}

// ============================================================================
// MqttNumericDataPoint Implementation
// ============================================================================

// Define static members
SemaphoreHandle_t MqttNumericDataPoint::updateSemaphore = nullptr;
bool MqttNumericDataPoint::semaphoreInitialized = false;

MqttNumericDataPoint::MqttNumericDataPoint(int decimalPlaces,
                                           float significanceThreshold,
                                           const char* topicFormat,
                                           ...)
    : MqttDataPoint(),
      updatePending(false),
      currentValue(0.0f),
      lastReportedValue(0.0f),
      significanceThreshold(significanceThreshold),
      decimalPlaces(decimalPlaces),
      hasValue(false),
      pendingReport(false)
{
    // Initialize per-class static update semaphore once. Concurrent
    // first-construction is safe: each thread creates a candidate mutex,
    // then under the global ListLock only the first installed wins; the
    // other(s) free their unused candidate.
    if (!semaphoreInitialized) {
        SemaphoreHandle_t fresh = xSemaphoreCreateMutex();
        {
            ListLock lock;
            if (!semaphoreInitialized) {
                updateSemaphore = fresh;
                semaphoreInitialized = true;
                fresh = nullptr;
            }
        }
        if (fresh) vSemaphoreDelete(fresh);
    }

    // Initialize update buffer
    updateBuffer.value = 0.0f;
    updateBuffer.expirationSeconds = 0;
    updateBuffer.isNonExpiring = false;

    // Format and set the topic
    va_list args;
    va_start(args, topicFormat);
    setFormattedTopic(topicFormat, args);
    va_end(args);
}

float MqttNumericDataPoint::roundToDecimalPlaces(float value) const {
    // Calculate multiplier based on decimal places
    float multiplier = 1.0f;
    for (int i = 0; i < decimalPlaces; i++) {
        multiplier *= 10.0f;
    }

    // Round to specified decimal places
    return round(value * multiplier) / multiplier;
}

void MqttNumericDataPoint::updateValue(float value, unsigned long expirationSeconds) {
    // Called from any thread; the semaphore + buffer hand off the new reading
    // to the main thread, which drains it from tick() via syncFromUpdateBuffer().
    if (updateSemaphore != nullptr && xSemaphoreTake(updateSemaphore, portMAX_DELAY) == pdTRUE) {
        updateBuffer.value = roundToDecimalPlaces(value);
        updateBuffer.expirationSeconds = expirationSeconds;
        updateBuffer.isNonExpiring = (expirationSeconds == 0);

        updatePending = true;

        xSemaphoreGive(updateSemaphore);
    }
}

void MqttNumericDataPoint::updateNonExpiringValue(float value) {
    updateValue(value, 0);  // 0 expirationSeconds = never expires
}

void MqttNumericDataPoint::syncFromUpdateBuffer() {
    if (updatePending) {
        if (updateSemaphore != nullptr && xSemaphoreTake(updateSemaphore, portMAX_DELAY) == pdTRUE) {
            currentValue = updateBuffer.value;
            lastSensorUpdate = millis();

            if (updateBuffer.isNonExpiring) {
                expiresAtMillis = 0;  // 0 = never expires
            } else {
                expiresAtMillis = lastSensorUpdate + (updateBuffer.expirationSeconds * 1000UL);
            }

            sensorValid = true;
            hasValue = true;

            // Both currentValue and lastReportedValue are already rounded, so
            // this comparison is clean.
            if (hasBeenReported) {
                float absoluteChange = abs(currentValue - lastReportedValue);
                if (absoluteChange >= significanceThreshold) {
                    pendingReport = true;
                } else {
                    pendingReport = false;
                }
            }

            updatePending = false;

            xSemaphoreGive(updateSemaphore);
        }
    }
}

bool MqttNumericDataPoint::isChangeSignificant() const {
    if (!hasBeenReported) {
        return true;  // First report is always significant
    }

    float absoluteChange = abs(currentValue - lastReportedValue);
    return absoluteChange >= significanceThreshold;
}

bool MqttNumericDataPoint::needsReport() const {
    if (!hasValidValue()) {
        return false;
    }

    if (forceReport) {
        return true;
    }

    unsigned long now = millis();

    if (!hasBeenReported) {
        return true;
    }

    if (lastUnsuccessfulReport > lastSuccessfulReport) {
        if ((now - lastUnsuccessfulReport) >= 5000) {
            return true;
        }
    }

    if (pendingReport) {
        if ((now - lastSuccessfulReport) >= (changingReportIntervalSeconds * 1000UL)) {
            return true;
        }
    }

    if (isChangeSignificant()) {
        if ((now - lastSuccessfulReport) >= (changingReportIntervalSeconds * 1000UL)) {
            return true;
        }
    } else {
        if ((now - lastSuccessfulReport) >= (stableReportIntervalSeconds * 1000UL)) {
            return true;
        }
    }

    return false;
}

bool MqttNumericDataPoint::doReport() {
    if (!hasValidValue()) {
        return false;
    }

    char payload[32];
    dtostrf(currentValue, 1, decimalPlaces, payload);

    bool success = attemptPublish(payload);  // retained=true by default

    if (success) {
        lastReportedValue = currentValue;
        pendingReport = false;
        forceReport = false;
    }

    return success;
}

bool MqttNumericDataPoint::tick() {
    syncFromUpdateBuffer();

    if (isDataExpired()) {
        sensorValid = false;
    }

    if (NetClient.isMqttConnected() && needsReport()) {
        return doReport();
    }

    return false;
}

// ============================================================================
// MqttStringDataPoint Implementation
// ============================================================================

// Define static members
SemaphoreHandle_t MqttStringDataPoint::updateSemaphore = nullptr;
bool MqttStringDataPoint::semaphoreInitialized = false;

MqttStringDataPoint::MqttStringDataPoint(const char* topicFormat, ...)
    : MqttDataPoint(),
      updatePending(false),
      hasValue(false),
      pendingReport(false),
      lastValueChange(0),
      reportCount(0),
      reportIndex(0),
      firstChangeTime(0),
      changeCount(0)
{
    if (!semaphoreInitialized) {
        SemaphoreHandle_t fresh = xSemaphoreCreateMutex();
        {
            ListLock lock;
            if (!semaphoreInitialized) {
                updateSemaphore = fresh;
                semaphoreInitialized = true;
                fresh = nullptr;
            }
        }
        if (fresh) vSemaphoreDelete(fresh);
    }

    updateBuffer.value[0] = '\0';
    updateBuffer.expirationSeconds = 0;
    updateBuffer.isNonExpiring = false;

    va_list args;
    va_start(args, topicFormat);
    setFormattedTopic(topicFormat, args);
    va_end(args);

    currentValue[0] = '\0';
    lastReportedValue[0] = '\0';

    for (int i = 0; i < MAX_REPORTS_PER_WINDOW; i++) {
        reportTimestamps[i] = 0;
    }
}

void MqttStringDataPoint::updateValue(const char* value, unsigned long expirationSeconds) {
    if (updateSemaphore != nullptr && xSemaphoreTake(updateSemaphore, portMAX_DELAY) == pdTRUE) {
        strncpy(updateBuffer.value, value, sizeof(updateBuffer.value) - 1);
        updateBuffer.value[sizeof(updateBuffer.value) - 1] = '\0';
        updateBuffer.expirationSeconds = expirationSeconds;
        updateBuffer.isNonExpiring = (expirationSeconds == 0);

        updatePending = true;

        xSemaphoreGive(updateSemaphore);
    }
}

void MqttStringDataPoint::updateNonExpiringValue(const char* value) {
    updateValue(value, 0);
}

void MqttStringDataPoint::syncFromUpdateBuffer() {
    if (updatePending) {
        if (updateSemaphore != nullptr && xSemaphoreTake(updateSemaphore, portMAX_DELAY) == pdTRUE) {
            bool isChange = false;
            if (hasValue) {
                isChange = (strcmp(currentValue, updateBuffer.value) != 0);
            } else {
                isChange = true;
            }

            strncpy(currentValue, updateBuffer.value, sizeof(currentValue) - 1);
            currentValue[sizeof(currentValue) - 1] = '\0';

            lastSensorUpdate = millis();

            if (updateBuffer.isNonExpiring) {
                expiresAtMillis = 0;
            } else {
                expiresAtMillis = lastSensorUpdate + (updateBuffer.expirationSeconds * 1000UL);
            }

            sensorValid = true;
            hasValue = true;

            if (isChange) {
                lastValueChange = millis();
            }

            if (hasBeenReported) {
                if (strcmp(currentValue, lastReportedValue) != 0) {
                    pendingReport = true;
                } else {
                    pendingReport = false;
                }
            }

            if (isChange) {
                unsigned long now = millis();

                if (changeCount == 0 || (now - firstChangeTime) > RAPID_CHANGE_WINDOW) {
                    firstChangeTime = now;
                    changeCount = 1;
                } else {
                    changeCount++;
                }
            }

            updatePending = false;

            xSemaphoreGive(updateSemaphore);
        }
    }
}

bool MqttStringDataPoint::hasStateChanged() const {
    if (!hasBeenReported) {
        return true;
    }
    return strcmp(currentValue, lastReportedValue) != 0;
}

bool MqttStringDataPoint::isRapidlyChanging() const {
    unsigned long now = millis();

    if (changeCount > RAPID_CHANGE_THRESHOLD &&
        (now - firstChangeTime) <= RAPID_CHANGE_WINDOW) {
        return true;
    }

    return false;
}

bool MqttStringDataPoint::canReport() const {
    unsigned long now = millis();
    int recentReports = 0;

    for (int i = 0; i < MAX_REPORTS_PER_WINDOW; i++) {
        if (reportTimestamps[i] > 0 && (now - reportTimestamps[i]) <= REPORT_WINDOW_MS) {
            recentReports++;
        }
    }

    return recentReports < MAX_REPORTS_PER_WINDOW;
}

void MqttStringDataPoint::recordReport() {
    reportTimestamps[reportIndex] = millis();
    reportIndex = (reportIndex + 1) % MAX_REPORTS_PER_WINDOW;

    if (reportCount < MAX_REPORTS_PER_WINDOW) {
        reportCount++;
    }
}

bool MqttStringDataPoint::needsReport() const {
    if (!hasValidValue()) {
        return false;
    }

    if (forceReport) {
        return canReport();
    }

    unsigned long now = millis();

    if (!hasBeenReported) {
        return canReport();
    }

    if (lastUnsuccessfulReport > lastSuccessfulReport) {
        if ((now - lastUnsuccessfulReport) >= 5000) {
            return canReport();
        }
    }

    if (pendingReport) {
        // Final report after stabilization: if the value has been stable for
        // FINAL_REPORT_STABILITY_MS, allow one final report even if rate
        // limited so we don't get stuck on the wrong value after rapid change.
        if (lastValueChange > 0 && (now - lastValueChange) >= FINAL_REPORT_STABILITY_MS) {
            if (isRapidlyChanging()) {
                if ((now - lastSuccessfulReport) < 3000) {
                    return false;
                }
            }
            return true;
        }

        if (!canReport()) {
            return false;
        }

        if (isRapidlyChanging()) {
            if ((now - lastSuccessfulReport) < 3000) {
                return false;
            }
        }

        return true;
    }

    if (hasStateChanged()) {
        if (!canReport()) {
            return false;
        }

        if (isRapidlyChanging()) {
            if ((now - lastSuccessfulReport) < 3000) {
                return false;
            }
        }

        return true;
    }

    if ((now - lastSuccessfulReport) >= (stableReportIntervalSeconds * 1000UL)) {
        if (canReport()) {
            return true;
        }
    }

    return false;
}

bool MqttStringDataPoint::doReport() {
    if (!hasValidValue()) {
        return false;
    }

    bool success = attemptPublish(currentValue);

    if (success) {
        strncpy(lastReportedValue, currentValue, sizeof(lastReportedValue) - 1);
        lastReportedValue[sizeof(lastReportedValue) - 1] = '\0';
        recordReport();
        pendingReport = false;
        forceReport = false;
    }

    return success;
}

bool MqttStringDataPoint::tick() {
    syncFromUpdateBuffer();

    if (isDataExpired()) {
        sensorValid = false;
    }

    if (NetClient.isMqttConnected() && needsReport()) {
        return doReport();
    }

    return false;
}

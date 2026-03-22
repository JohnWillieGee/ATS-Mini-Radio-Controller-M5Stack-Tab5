// =============================================================================
// radio_state.cpp
// =============================================================================
#include "radio_state.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CSV_FIELD_COUNT 15
#define LINE_BUF_SIZE   256

bool radio_parse_csv(const char *line, RadioState &r) {
    static char buf[LINE_BUF_SIZE];
    strncpy(buf, line, LINE_BUF_SIZE - 1);
    buf[LINE_BUF_SIZE - 1] = '\0';

    char *fields[CSV_FIELD_COUNT];
    int   count = 0;
    char *p = buf;

    while (count < CSV_FIELD_COUNT) {
        fields[count++] = p;
        p = strchr(p, ',');
        if (!p) break;
        *p++ = '\0';
    }

    if (count < CSV_FIELD_COUNT) return false;

    r.appVersion   = atoi(fields[0]);
    r.rawFrequency = atol(fields[1]);
    r.bfo          = atoi(fields[2]);
    r.bandCal      = atoi(fields[3]);
    strncpy(r.bandName, fields[4], sizeof(r.bandName) - 1);
    r.bandName[sizeof(r.bandName) - 1] = '\0';
    strncpy(r.modeName, fields[5], sizeof(r.modeName) - 1);
    r.modeName[sizeof(r.modeName) - 1] = '\0';
    r.stepIdx      = atoi(fields[6]);
    r.bandwidthIdx = atoi(fields[7]);
    r.agcIdx       = atoi(fields[8]);
    r.volume       = atoi(fields[9]);
    r.rssi         = atoi(fields[10]);
    r.snr          = atoi(fields[11]);
    r.tuningCap    = atoi(fields[12]);
    int rawV       = atoi(fields[13]);
    r.batteryVoltage = rawV * 1.702f / 1000.0f;
    r.seqNum       = atoi(fields[14]);

    // Derive display frequency in Hz
    if (strcmp(r.modeName, "FM") == 0) {
        r.displayFreqHz = (long)r.rawFrequency * 10000L;
    } else if (strcmp(r.modeName, "LSB") == 0 || strcmp(r.modeName, "USB") == 0) {
        r.displayFreqHz = ((long)r.rawFrequency * 1000L) + r.bfo;
    } else {
        r.displayFreqHz = (long)r.rawFrequency * 1000L;
    }

    r.valid = true;
    return true;
}

void radio_freq_string(const RadioState &r, char *buf, size_t buflen) {
    long hz = r.displayFreqHz;
    if (hz >= 1000000L) {
        // MHz range — FM or HF
        float mhz = hz / 1000000.0f;
        if (strcmp(r.modeName, "FM") == 0) {
            snprintf(buf, buflen, "%.2f MHz", mhz);
        } else {
            snprintf(buf, buflen, "%.3f MHz", mhz);
        }
    } else if (hz >= 1000L) {
        snprintf(buf, buflen, "%.3f kHz", hz / 1000.0f);
    } else {
        snprintf(buf, buflen, "%ld Hz", hz);
    }
}

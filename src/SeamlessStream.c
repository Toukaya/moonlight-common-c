/**
 * @file src/SeamlessStream.c
 * @brief Encoder, decoder and validation for the seamless window channel.
 * @details Nothing here changes behaviour on its own. Every message is length
 *          checked against its documented fixed size before a single payload
 *          field is read, and the table messages are checked against their
 *          limits and their checksum before any receiver is called.
 */
#include "Limelight-internal.h"
#include "SeamlessStream.h"

#include <stdlib.h>
#include <string.h>

static LiSeamlessCallbacks seamlessCallbacks;
static void *seamlessCallbackContext;
static bool seamlessEnabled;

void LiSetSeamlessEnabled(int enabled) {
    seamlessEnabled = (enabled != 0);
    Limelog("Seamless channel %s\n", seamlessEnabled ? "enabled" : "disabled");
}

int LiIsSeamlessEnabled(void) {
    return seamlessEnabled ? 1 : 0;
}

// Reads a decimal attribute value out of a session description written in the
// "a=name:value" form. Returns 1 when the attribute is present and parses.
static int read_sdp_attribute(const char* description, const char* name, uint32_t* value) {
    const char* found;
    char* end = NULL;
    unsigned long parsed;

    if (description == NULL || name == NULL || value == NULL) {
        return 0;
    }
    found = strstr(description, name);
    if (found == NULL) {
        return 0;
    }
    found += strlen(name);
    if (*found != ':') {
        return 0;
    }
    found++;
    while (*found == ' ' || *found == '\t') {
        found++;
    }
    parsed = strtoul(found, &end, 10);
    if (end == found) {
        return 0;
    }
    *value = (uint32_t)parsed;
    return 1;
}

int LiSeamlessEvaluateHostAnswer(int requested, const char* description) {
    uint32_t answer = 0;

    if (!requested) {
        LiSetSeamlessEnabled(0);
        return 0;
    }
    if (!read_sdp_attribute(description, "x-ss-general.seamless", &answer) || answer == 0) {
        LiSetSeamlessEnabled(0);
        Limelog("Host did not answer the seamless window capability\n");
        return 0;
    }
    LiSetSeamlessEnabled(1);
    return 1;
}

void LiSetSeamlessCallbacks(const LiSeamlessCallbacks *callbacks, void *context) {
    if (callbacks != NULL) {
        seamlessCallbacks = *callbacks;
    }
    else {
        memset(&seamlessCallbacks, 0, sizeof(seamlessCallbacks));
    }
    seamlessCallbackContext = context;
}

// Little endian field writers and readers. The wire format is little endian
// on every platform, independent of the host byte order.
static uint8_t *wr8(uint8_t *p, uint8_t v) {
    *p = v;
    return p + 1;
}

static uint8_t *wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    return p + 2;
}

static uint8_t *wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
    return p + 4;
}

static uint8_t *wrEntry(uint8_t *p, const LiSeamlessWindowEntry *e) {
    p = wr32(p, e->id);
    p = wr16(p, e->cellX);
    p = wr16(p, e->cellY);
    p = wr16(p, e->cellW);
    p = wr16(p, e->cellH);
    p = wr16(p, e->clientW);
    p = wr16(p, e->clientH);
    p = wr16(p, (uint16_t)e->desktopX);
    p = wr16(p, (uint16_t)e->desktopY);
    p = wr8(p, e->z);
    p = wr8(p, e->flags);
    // Two reserved bytes, always written as zero. They are what makes the wire
    // entry 32 bytes wide in the same shape as the in-memory entry, so the
    // stride of the wire and the stride of the struct cannot disagree.
    p = wr8(p, 0);
    p = wr8(p, 0);
    p = wr32(p, e->titleHash);
    p = wr32(p, e->iconHash);
    return p;
}

static uint8_t rd8(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint8_t v = p[0];
    *pp = p + 1;
    return v;
}

static uint16_t rd16(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint16_t v = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    *pp = p + 2;
    return v;
}

static uint32_t rd32(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *pp = p + 4;
    return v;
}

static void rdEntry(const uint8_t **pp, LiSeamlessWindowEntry *e) {
    e->id = rd32(pp);
    e->cellX = rd16(pp);
    e->cellY = rd16(pp);
    e->cellW = rd16(pp);
    e->cellH = rd16(pp);
    e->clientW = rd16(pp);
    e->clientH = rd16(pp);
    e->desktopX = (int16_t)rd16(pp);
    e->desktopY = (int16_t)rd16(pp);
    e->z = rd8(pp);
    e->flags = rd8(pp);
    rd8(pp);  // reserved, ignored on receive
    rd8(pp);
    e->titleHash = rd32(pp);
    e->iconHash = rd32(pp);
}

// Checksum over the entry bytes, the standard reflected form with the usual
// polynomial. Both ends compute it over exactly the same byte range.
static uint32_t seamlessCrc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static int sendOnSeamlessChannel(const uint8_t *payload, int length) {
    if (!seamlessEnabled) {
        return -2;
    }
    return sendControlMessageOnChannel(SEAMLESS_CHANNEL_ID, payload, length);
}

int LiSendSeamlessMessage(const uint8_t *payload, int length) {
    return sendOnSeamlessChannel(payload, length);
}

static int validateEntry(const LiSeamlessWindowEntry *e, uint16_t canvasW, uint16_t canvasH) {
    if (e->cellW == 0 || e->cellH == 0 || e->clientW == 0 || e->clientH == 0) {
        return 0;
    }
    if ((uint32_t)e->cellX + e->cellW > canvasW) {
        return 0;
    }
    if ((uint32_t)e->cellY + e->cellH > canvasH) {
        return 0;
    }
    return 1;
}

static int handleTable(int isSnapshot, const uint8_t *payload, int length) {
    const uint8_t *p;
    int headerSize;
    uint8_t type;
    uint8_t seq;
    uint8_t mask = 0;
    uint8_t count;
    uint16_t canvasW = 0;
    uint16_t canvasH = 0;
    uint32_t crc = 0;
    int16_t workAreaX;
    int16_t workAreaY;
    uint16_t workAreaW;
    uint16_t workAreaH;
    LiSeamlessWindowEntry entries[SEAMLESS_MAX_WINDOWS];
    int needed;
    int i;

    headerSize = isSnapshot ? SEAMLESS_SNAPSHOT_HEADER_SIZE : SEAMLESS_DELTA_HEADER_SIZE;
    if (length < headerSize) {
        Limelog("Dropping seamless table message: %d < %d\n", length, headerSize);
        return -1;
    }

    p = payload;
    type = rd8(&p);
    seq = rd8(&p);
    if (isSnapshot) {
        count = rd8(&p);
        canvasW = rd16(&p);
        canvasH = rd16(&p);
        crc = rd32(&p);
    }
    else {
        mask = rd8(&p);
        count = rd8(&p);
    }
    workAreaX = (int16_t)rd16(&p);
    workAreaY = (int16_t)rd16(&p);
    workAreaW = rd16(&p);
    workAreaH = rd16(&p);

    if (type != (isSnapshot ? SEAMLESS_MSG_WINDOW_SNAPSHOT : SEAMLESS_MSG_WINDOW_DELTA)) {
        Limelog("Dropping seamless table message: type %u does not match %d\n", type, isSnapshot);
        return -1;
    }
    if (count > SEAMLESS_MAX_WINDOWS) {
        Limelog("Dropping seamless table message: count %u > %d\n", count, SEAMLESS_MAX_WINDOWS);
        return -1;
    }

    needed = headerSize + ((int)count * SEAMLESS_ENTRY_SIZE);
    if (length != needed) {
        Limelog("Dropping seamless table message: %d != %d\n", length, needed);
        return -1;
    }

    if (isSnapshot) {
        if (canvasW > SEAMLESS_MAX_CANVAS || canvasH > SEAMLESS_MAX_CANVAS) {
            Limelog("Dropping seamless table message: canvas %ux%u over the maximum\n", canvasW, canvasH);
            return -1;
        }
        if (seamlessCrc32(payload + headerSize, (size_t)count * SEAMLESS_ENTRY_SIZE) != crc) {
            Limelog("Dropping seamless table message: checksum mismatch\n");
            return -1;
        }
    }

    for (i = 0; i < count; i++) {
        rdEntry(&p, &entries[i]);
        if (isSnapshot && !validateEntry(&entries[i], canvasW, canvasH)) {
            Limelog("Dropping seamless table message: entry %d out of bounds\n", i);
            return -1;
        }
    }

    Limelog("Seamless %s seq=%u count=%u mask=0x%02x workarea=%d,%d %ux%u\n",
            isSnapshot ? "snapshot" : "delta", seq, count, mask,
            workAreaX, workAreaY, workAreaW, workAreaH);

    if (seamlessCallbacks.table != NULL) {
        seamlessCallbacks.table(seq, isSnapshot ? SEAMLESS_TABLE_SNAPSHOT : SEAMLESS_TABLE_DELTA,
                                canvasW, canvasH, workAreaX, workAreaY,
                                workAreaW, workAreaH, entries, count,
                                seamlessCallbackContext);
    }
    return 0;
}

int LiSeamlessHandleMessage(const uint8_t *payload, int length) {
    const uint8_t *p;
    uint8_t type;

    if (!seamlessEnabled) {
        return -2;
    }
    if (payload == NULL || length < 1) {
        return -1;
    }

    p = payload;
    type = rd8(&p);

    switch (type) {
    case SEAMLESS_MSG_WINDOW_SNAPSHOT:
        return handleTable(1, payload, length);

    case SEAMLESS_MSG_WINDOW_DELTA:
        return handleTable(0, payload, length);

    case SEAMLESS_MSG_ICON:
        {
            uint32_t iconHash;
            uint16_t iconLength;

            if (length < SEAMLESS_ICON_HEADER_SIZE) {
                Limelog("Dropping seamless icon: %d < %d\n", length, SEAMLESS_ICON_HEADER_SIZE);
                return -1;
            }
            iconHash = rd32(&p);
            iconLength = rd16(&p);
            if (iconLength == 0 || iconLength > SEAMLESS_MAX_ICON) {
                Limelog("Dropping seamless icon: length %u over the maximum\n", iconLength);
                return -1;
            }
            if (length != SEAMLESS_ICON_HEADER_SIZE + (int)iconLength) {
                Limelog("Dropping seamless icon: %d != %d\n", length,
                        SEAMLESS_ICON_HEADER_SIZE + (int)iconLength);
                return -1;
            }
            Limelog("Seamless icon hash=0x%08x length=%u\n", iconHash, iconLength);
            if (seamlessCallbacks.icon != NULL) {
                seamlessCallbacks.icon(iconHash, p, (int)iconLength, seamlessCallbackContext);
            }
            return 0;
        }

    case SEAMLESS_MSG_CARET_POSITION:
        {
            uint8_t windowId;
            int16_t x;
            int16_t y;
            uint16_t height;

            if (length != SEAMLESS_CARET_SIZE) {
                Limelog("Dropping seamless caret: %d != %d\n", length, SEAMLESS_CARET_SIZE);
                return -1;
            }
            windowId = rd8(&p);
            x = (int16_t)rd16(&p);
            y = (int16_t)rd16(&p);
            height = rd16(&p);
            Limelog("Seamless caret window=%u at %d,%d height=%u\n", windowId, x, y, height);
            if (seamlessCallbacks.caret != NULL) {
                seamlessCallbacks.caret(windowId, x, y, height, seamlessCallbackContext);
            }
            return 0;
        }

    case SEAMLESS_MSG_WINDOW_INTENT:
        {
            uint8_t windowId;
            uint8_t kind;
            int16_t x;
            int16_t y;
            uint16_t width;
            uint16_t height;

            if (length != SEAMLESS_INTENT_SIZE) {
                Limelog("Dropping seamless intent: %d != %d\n", length, SEAMLESS_INTENT_SIZE);
                return -1;
            }
            windowId = rd8(&p);
            kind = rd8(&p);
            x = (int16_t)rd16(&p);
            y = (int16_t)rd16(&p);
            width = rd16(&p);
            height = rd16(&p);
            if (kind > SEAMLESS_INTENT_STICKY) {
                Limelog("Dropping seamless intent: kind %u unknown\n", kind);
                return -1;
            }
            Limelog("Seamless intent window=%u kind=%u rect=%d,%d %ux%u\n",
                    windowId, kind, x, y, width, height);
            if (seamlessCallbacks.intent != NULL) {
                seamlessCallbacks.intent(windowId, kind, x, y, width, height,
                                         seamlessCallbackContext);
            }
            return 0;
        }

    case SEAMLESS_MSG_WINDOW_INPUT:
        {
            uint8_t windowId;
            uint8_t mode;
            uint32_t hostPid;

            if (length != SEAMLESS_INPUT_SIZE) {
                Limelog("Dropping seamless input: %d != %d\n", length, SEAMLESS_INPUT_SIZE);
                return -1;
            }
            windowId = rd8(&p);
            mode = rd8(&p);
            hostPid = rd32(&p);
            Limelog("Seamless input window=%u mode=%u pid=%u\n", windowId, mode, hostPid);
            if (seamlessCallbacks.input != NULL) {
                seamlessCallbacks.input(windowId, mode, hostPid, seamlessCallbackContext);
            }
            return 0;
        }

    case SEAMLESS_MSG_WINDOW_ACK:
        {
            uint8_t seq;
            uint8_t result;

            if (length != SEAMLESS_ACK_SIZE) {
                Limelog("Dropping seamless ack: %d != %d\n", length, SEAMLESS_ACK_SIZE);
                return -1;
            }
            seq = rd8(&p);
            result = rd8(&p);
            Limelog("Seamless ack seq=%u result=%u\n", seq, result);
            if (seamlessCallbacks.ack != NULL) {
                seamlessCallbacks.ack(seq, result, seamlessCallbackContext);
            }
            return 0;
        }

    case SEAMLESS_MSG_PRESENT:
        {
            uint32_t frameId;
            uint32_t presentUs;

            if (length != SEAMLESS_PRESENT_SIZE) {
                Limelog("Dropping seamless present: %d != %d\n", length, SEAMLESS_PRESENT_SIZE);
                return -1;
            }
            frameId = rd32(&p);
            presentUs = rd32(&p);
            Limelog("Seamless present frame=%u at %u us\n", frameId, presentUs);
            if (seamlessCallbacks.present != NULL) {
                seamlessCallbacks.present(frameId, presentUs, seamlessCallbackContext);
            }
            return 0;
        }

    case SEAMLESS_MSG_SKIP:
        {
            uint32_t frameId;
            uint8_t reason;

            if (length != SEAMLESS_SKIP_SIZE) {
                Limelog("Dropping seamless skip: %d != %d\n", length, SEAMLESS_SKIP_SIZE);
                return -1;
            }
            frameId = rd32(&p);
            reason = rd8(&p);
            Limelog("Seamless skip frame=%u reason=%u\n", frameId, reason);
            if (seamlessCallbacks.skip != NULL) {
                seamlessCallbacks.skip(frameId, reason, seamlessCallbackContext);
            }
            return 0;
        }

    default:
        Limelog("Dropping seamless message: unknown type 0x%02x\n", type);
        return -1;
    }
}

int LiEncodeWindowSnapshot(uint8_t *buffer, int capacity,
                           uint8_t seq, uint16_t canvasW, uint16_t canvasH,
                           int16_t workAreaX, int16_t workAreaY,
                           uint16_t workAreaW, uint16_t workAreaH,
                           const LiSeamlessWindowEntry *entries, int count) {
    uint8_t *p = buffer;
    int i;
    uint32_t crc;
    int length;

    if (buffer == NULL || (entries == NULL && count > 0)) {
        return -1;
    }
    if (count < 0 || count > SEAMLESS_MAX_WINDOWS) {
        return -1;
    }
    if (canvasW > SEAMLESS_MAX_CANVAS || canvasH > SEAMLESS_MAX_CANVAS) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!validateEntry(&entries[i], canvasW, canvasH)) {
            return -1;
        }
    }

    length = SEAMLESS_SNAPSHOT_HEADER_SIZE + (count * SEAMLESS_ENTRY_SIZE);
    if (capacity < length) {
        return -1;
    }

    p = wr8(p, SEAMLESS_MSG_WINDOW_SNAPSHOT);
    p = wr8(p, seq);
    p = wr8(p, (uint8_t)count);
    p = wr16(p, canvasW);
    p = wr16(p, canvasH);
    p = wr32(p, 0);  // checksum filled below, its position is fixed
    p = wr16(p, (uint16_t)workAreaX);
    p = wr16(p, (uint16_t)workAreaY);
    p = wr16(p, workAreaW);
    p = wr16(p, workAreaH);
    for (i = 0; i < count; i++) {
        p = wrEntry(p, &entries[i]);
    }

    crc = seamlessCrc32(buffer + SEAMLESS_SNAPSHOT_HEADER_SIZE, (size_t)count * SEAMLESS_ENTRY_SIZE);
    buffer[7] = (uint8_t)(crc & 0xFF);
    buffer[8] = (uint8_t)((crc >> 8) & 0xFF);
    buffer[9] = (uint8_t)((crc >> 16) & 0xFF);
    buffer[10] = (uint8_t)((crc >> 24) & 0xFF);

    return length;
}

int LiSendWindowSnapshot(uint8_t seq, uint16_t canvasW, uint16_t canvasH,
                         int16_t workAreaX, int16_t workAreaY,
                         uint16_t workAreaW, uint16_t workAreaH,
                         const LiSeamlessWindowEntry *entries, int count) {
    uint8_t buffer[SEAMLESS_SNAPSHOT_HEADER_SIZE + (SEAMLESS_MAX_WINDOWS * SEAMLESS_ENTRY_SIZE)];
    int length = LiEncodeWindowSnapshot(buffer, (int)sizeof(buffer), seq, canvasW, canvasH,
                                        workAreaX, workAreaY, workAreaW, workAreaH,
                                        entries, count);
    if (length < 0) {
        return -1;
    }
    return sendOnSeamlessChannel(buffer, length);
}

int LiEncodeWindowDelta(uint8_t *buffer, int capacity,
                        uint8_t seq, uint8_t mask,
                        int16_t workAreaX, int16_t workAreaY,
                        uint16_t workAreaW, uint16_t workAreaH,
                        const LiSeamlessWindowEntry *entries, int count) {
    uint8_t *p = buffer;
    int i;
    int length;

    if (buffer == NULL || (entries == NULL && count > 0)) {
        return -1;
    }
    if (count < 0 || count > SEAMLESS_MAX_WINDOWS) {
        return -1;
    }

    length = SEAMLESS_DELTA_HEADER_SIZE + (count * SEAMLESS_ENTRY_SIZE);
    if (capacity < length) {
        return -1;
    }

    p = wr8(p, SEAMLESS_MSG_WINDOW_DELTA);
    p = wr8(p, seq);
    p = wr8(p, mask);
    p = wr8(p, (uint8_t)count);
    p = wr16(p, (uint16_t)workAreaX);
    p = wr16(p, (uint16_t)workAreaY);
    p = wr16(p, workAreaW);
    p = wr16(p, workAreaH);
    for (i = 0; i < count; i++) {
        p = wrEntry(p, &entries[i]);
    }

    return length;
}

int LiSendWindowDelta(uint8_t seq, uint8_t mask,
                      int16_t workAreaX, int16_t workAreaY,
                      uint16_t workAreaW, uint16_t workAreaH,
                      const LiSeamlessWindowEntry *entries, int count) {
    uint8_t buffer[SEAMLESS_DELTA_HEADER_SIZE + (SEAMLESS_MAX_WINDOWS * SEAMLESS_ENTRY_SIZE)];
    int length = LiEncodeWindowDelta(buffer, (int)sizeof(buffer), seq, mask,
                                     workAreaX, workAreaY, workAreaW, workAreaH,
                                     entries, count);
    if (length < 0) {
        return -1;
    }
    return sendOnSeamlessChannel(buffer, length);
}

int LiSendSeamlessIcon(uint32_t iconHash, const uint8_t *data, int length) {
    uint8_t buffer[SEAMLESS_ICON_HEADER_SIZE + SEAMLESS_MAX_ICON];
    uint8_t *p = buffer;

    if (data == NULL || length <= 0 || length > SEAMLESS_MAX_ICON) {
        return -1;
    }

    p = wr8(p, SEAMLESS_MSG_ICON);
    p = wr32(p, iconHash);
    p = wr16(p, (uint16_t)length);
    memcpy(p, data, (size_t)length);

    return sendOnSeamlessChannel(buffer, SEAMLESS_ICON_HEADER_SIZE + length);
}

int LiSendCaretPosition(uint8_t windowId, int16_t x, int16_t y, uint16_t height) {
    uint8_t buffer[SEAMLESS_CARET_SIZE];
    uint8_t *p = buffer;

    p = wr8(p, SEAMLESS_MSG_CARET_POSITION);
    p = wr8(p, windowId);
    p = wr16(p, (uint16_t)x);
    p = wr16(p, (uint16_t)y);
    p = wr16(p, height);

    return sendOnSeamlessChannel(buffer, SEAMLESS_CARET_SIZE);
}

int LiSendWindowAck(uint8_t seq, uint8_t result) {
    uint8_t buffer[SEAMLESS_ACK_SIZE];
    uint8_t *p = buffer;

    p = wr8(p, SEAMLESS_MSG_WINDOW_ACK);
    p = wr8(p, seq);
    p = wr8(p, result);

    return sendOnSeamlessChannel(buffer, SEAMLESS_ACK_SIZE);
}

int LiSendWindowInput(uint8_t windowId, uint8_t mode, uint32_t hostPid) {
    uint8_t buffer[SEAMLESS_INPUT_SIZE];
    uint8_t *p = buffer;

    p = wr8(p, SEAMLESS_MSG_WINDOW_INPUT);
    p = wr8(p, windowId);
    p = wr8(p, mode);
    p = wr32(p, hostPid);

    return sendOnSeamlessChannel(buffer, SEAMLESS_INPUT_SIZE);
}

int LiSendWindowIntent(uint8_t windowId, uint8_t kind, int16_t x, int16_t y,
                       uint16_t width, uint16_t height) {
    uint8_t buffer[SEAMLESS_INTENT_SIZE];
    uint8_t *p = buffer;

    if (kind > SEAMLESS_INTENT_STICKY) {
        return -1;
    }

    p = wr8(p, SEAMLESS_MSG_WINDOW_INTENT);
    p = wr8(p, windowId);
    p = wr8(p, kind);
    p = wr16(p, (uint16_t)x);
    p = wr16(p, (uint16_t)y);
    p = wr16(p, width);
    p = wr16(p, height);

    return sendOnSeamlessChannel(buffer, SEAMLESS_INTENT_SIZE);
}

int LiSendPresent(uint32_t frameId, uint32_t presentUs) {
    uint8_t buffer[SEAMLESS_PRESENT_SIZE];
    uint8_t *p = buffer;

    p = wr8(p, SEAMLESS_MSG_PRESENT);
    p = wr32(p, frameId);
    p = wr32(p, presentUs);

    return sendOnSeamlessChannel(buffer, SEAMLESS_PRESENT_SIZE);
}

int LiSendSkip(uint32_t frameId, uint8_t reason) {
    uint8_t buffer[SEAMLESS_SKIP_SIZE];
    uint8_t *p = buffer;

    p = wr8(p, SEAMLESS_MSG_SKIP);
    p = wr32(p, frameId);
    p = wr8(p, reason);

    return sendOnSeamlessChannel(buffer, SEAMLESS_SKIP_SIZE);
}

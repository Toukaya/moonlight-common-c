/**
 * @file src/SeamlessStream.h
 * @brief Definitions for the seamless window control channel.
 * @details This header is the single definition of the wire format. Every
 *          message is little endian, every fixed field is one of the widths
 *          below, and every message states its exact size so that both ends
 *          can length check a message before reading any field of it.
 *
 *          Message ids 0x50 to 0x58 form one set:
 *            0x50 WindowSnapshot   host to client, table version, all entries
 *            0x51 WindowDelta      host to client, table version, changed entries
 *            0x52 WindowInput      client to host, focus request
 *            0x53 WindowAck        client to host, table version result
 *            0x54 Present          client to host, presented frame id
 *            0x55 Skip             client to host, dropped frame id
 *            0x56 Icon             host to client, one icon blob by hash
 *            0x57 CaretPosition    host to client, text caret rectangle
 *            0x58 WindowIntent     client to host, local first move or resize
 *
 *          Table header, snapshot (19 bytes):
 *              u8  type            0x50
 *              u8  seq             table version, 0..255 wrapping
 *              u8  count           0..SEAMLESS_MAX_WINDOWS
 *              u16 canvasW
 *              u16 canvasH
 *              u32 crc32           over the entry bytes only
 *              i16 workAreaX       work area of the host virtual desktop
 *              i16 workAreaY
 *              u16 workAreaW
 *              u16 workAreaH
 *              WindowEntry entries[count]
 *
 *          Table header, delta (13 bytes):
 *              u8  type            0x51
 *              u8  seq
 *              u8  mask            SEAMLESS_DM_* groups carried below
 *              u8  count
 *              i16 workAreaX
 *              i16 workAreaY
 *              u16 workAreaW
 *              u16 workAreaH
 *              WindowEntry entries[count]
 *
 *          Entry, always 32 bytes on the wire, the width is checked at compile
 *          time against the sum of the field widths:
 *              u32 id
 *              u16 cellX, cellY, cellW, cellH
 *              u16 clientW, clientH
 *              i16 desktopX, desktopY
 *              u8  z
 *              u8  flags
 *              u8  reserved[2]     written as zero, ignored on receive
 *              u32 titleHash
 *              u32 iconHash
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The client offers this bit, the host answers the capability when it accepts.
#define SS_FF_SEAMLESS                 0x10

// Control channel id, next to the utf8 channel 0x06 and the server control
// channel 0x08. The payload type below is only a carrier; the channel id is
// what selects this message set.
#define SEAMLESS_CHANNEL_ID            0x07
#define SEAMLESS_PACKET_TYPE           0x5504

// Message ids.
#define SEAMLESS_MSG_WINDOW_SNAPSHOT   0x50
#define SEAMLESS_MSG_WINDOW_DELTA      0x51
#define SEAMLESS_MSG_WINDOW_INPUT      0x52
#define SEAMLESS_MSG_WINDOW_ACK        0x53
#define SEAMLESS_MSG_PRESENT           0x54
#define SEAMLESS_MSG_SKIP              0x55
#define SEAMLESS_MSG_ICON              0x56
#define SEAMLESS_MSG_CARET_POSITION    0x57
#define SEAMLESS_MSG_WINDOW_INTENT     0x58

// Limits, enforced before any payload byte is read or any buffer allocated.
#define SEAMLESS_MAX_WINDOWS           32
#define SEAMLESS_MAX_TITLE             256
#define SEAMLESS_MAX_ICON              8192
#define SEAMLESS_MAX_CANVAS            4096

// Exact message sizes in bytes, all of them fixed.
#define SEAMLESS_SNAPSHOT_HEADER_SIZE  19
#define SEAMLESS_DELTA_HEADER_SIZE     13
#define SEAMLESS_ENTRY_RESERVED        2
#define SEAMLESS_ENTRY_SIZE            32
#define SEAMLESS_ICON_HEADER_SIZE      7
#define SEAMLESS_CARET_SIZE            8
#define SEAMLESS_INTENT_SIZE           11
#define SEAMLESS_INPUT_SIZE            7
#define SEAMLESS_ACK_SIZE              3
#define SEAMLESS_PRESENT_SIZE          9
#define SEAMLESS_SKIP_SIZE             6

// Entry flags, bits 0 to 4.
#define SEAMLESS_WF_VISIBLE            0x01
#define SEAMLESS_WF_FOCUSED            0x02
#define SEAMLESS_WF_CAPTURABLE         0x04
#define SEAMLESS_WF_HAS_ICON           0x08
#define SEAMLESS_WF_HAS_TITLE          0x10

// Entry flags, bits 5 and 6 carry the window state, bit 7 is sticky.
#define SEAMLESS_WF_STATE_SHIFT        5
#define SEAMLESS_WF_STATE_MASK         0x60
#define SEAMLESS_WF_STICKY             0x80

#define SEAMLESS_WSTATE_NORMAL         0
#define SEAMLESS_WSTATE_MINIMIZED      1
#define SEAMLESS_WSTATE_MAXIMIZED      2
#define SEAMLESS_WSTATE_RESERVED       3

// Delta mask, one bit per field group.
#define SEAMLESS_DM_GEOMETRY           0x01
#define SEAMLESS_DM_ZORDER             0x02
#define SEAMLESS_DM_FLAGS              0x04
#define SEAMLESS_DM_TITLE              0x08
#define SEAMLESS_DM_ICON               0x10

// Acknowledgement results.
#define SEAMLESS_ACK_APPLIED           0
#define SEAMLESS_ACK_REJECTED          1
#define SEAMLESS_ACK_OVER_LIMIT        2

// Table versions, as the table callback receives them. A snapshot carries the
// whole set and replaces whatever the receiver holds; a delta carries only the
// entries that changed and must be merged into the held set. A receiver that
// cannot tell the two apart cannot apply a delta at all, so the kind is part of
// the callback and not something the receiver infers from the count.
#define SEAMLESS_TABLE_SNAPSHOT        0
#define SEAMLESS_TABLE_DELTA           1

// Skip reasons.
#define SEAMLESS_SKIP_LATE             0
#define SEAMLESS_SKIP_DECODE_ERROR     1
#define SEAMLESS_SKIP_REF_INVALID      2

// Local first intent kinds.
#define SEAMLESS_INTENT_MOVE           0
#define SEAMLESS_INTENT_RESIZE         1
#define SEAMLESS_INTENT_MAXIMIZE       2
#define SEAMLESS_INTENT_RESTORE        3
#define SEAMLESS_INTENT_STICKY         4

// Window input modes.
#define SEAMLESS_INPUT_FOCUS           0
#define SEAMLESS_INPUT_FOCUS_RAISE     1

// One table entry. The wire order is the field order.
typedef struct _LiSeamlessWindowEntry {
    uint32_t id;
    uint16_t cellX;
    uint16_t cellY;
    uint16_t cellW;
    uint16_t cellH;
    uint16_t clientW;
    uint16_t clientH;
    int16_t  desktopX;
    int16_t  desktopY;
    uint8_t  z;
    uint8_t  flags;
    uint32_t titleHash;
    uint32_t iconHash;
} LiSeamlessWindowEntry;

// The entry never grows: the wire format depends on this size. The check sums
// the field widths written by the encoder, so a struct whose compiler padding
// happens to match cannot hide a wire that does not.
#define SEAMLESS_ENTRY_WIRE_SIZE (4 + 2 + 2 + 2 + 2 + 2 + 2 + 2 + 2 + 1 + 1 + SEAMLESS_ENTRY_RESERVED + 4 + 4)
typedef char LiSeamlessEntrySizeCheck[(SEAMLESS_ENTRY_WIRE_SIZE == SEAMLESS_ENTRY_SIZE) ? 1 : -1];
typedef char LiSeamlessEntryNoTailPadding[(SEAMLESS_ENTRY_SIZE >= (int)sizeof(LiSeamlessWindowEntry)) ? 1 : -1];
typedef char LiSeamlessSnapshotHeaderCheck[(SEAMLESS_SNAPSHOT_HEADER_SIZE == 19) ? 1 : -1];
typedef char LiSeamlessIntentSizeCheck[(SEAMLESS_INTENT_SIZE == 11) ? 1 : -1];

// Receivers. Each one is called only after the message passed its size,
// limit and checksum checks. The table callback carries the kind of the
// version, because a snapshot replaces the receiver's set and a delta is
// merged into it.
typedef void (*LiSeamlessTableCallback)(uint8_t seq, uint8_t kind,
                                        uint16_t canvasW, uint16_t canvasH,
                                        int16_t workAreaX, int16_t workAreaY,
                                        uint16_t workAreaW, uint16_t workAreaH,
                                        const LiSeamlessWindowEntry *entries, int count,
                                        void *context);
typedef void (*LiSeamlessIconCallback)(uint32_t iconHash, const uint8_t *data, int length,
                                       void *context);
typedef void (*LiSeamlessCaretCallback)(uint8_t windowId, int16_t x, int16_t y, uint16_t height,
                                        void *context);
typedef void (*LiSeamlessAckCallback)(uint8_t seq, uint8_t result, void *context);
typedef void (*LiSeamlessInputCallback)(uint8_t windowId, uint8_t mode, uint32_t hostPid,
                                        void *context);
typedef void (*LiSeamlessIntentCallback)(uint8_t windowId, uint8_t kind, int16_t x, int16_t y,
                                         uint16_t width, uint16_t height, void *context);
typedef void (*LiSeamlessPresentCallback)(uint32_t frameId, uint32_t presentUs, void *context);
typedef void (*LiSeamlessSkipCallback)(uint32_t frameId, uint8_t reason, void *context);

typedef struct _LiSeamlessCallbacks {
    LiSeamlessTableCallback table;
    LiSeamlessIconCallback icon;
    LiSeamlessCaretCallback caret;
    LiSeamlessAckCallback ack;
    LiSeamlessInputCallback input;
    LiSeamlessIntentCallback intent;
    LiSeamlessPresentCallback present;
    LiSeamlessSkipCallback skip;
} LiSeamlessCallbacks;

// Negotiated flag. Nothing is sent and nothing is parsed while this is 0.
void LiSetSeamlessEnabled(int enabled);
// Applies a host description to the flag: the client asked (requested != 0) and
// the description carries x-ss-general.seamless with a value other than 0.
// Returns 1 when the channel is enabled, 0 when it is not. This is the whole
// client side decision, so it can be exercised without a live session.
int LiSeamlessEvaluateHostAnswer(int requested, const char* description);
int LiIsSeamlessEnabled(void);
void LiSetSeamlessCallbacks(const LiSeamlessCallbacks *callbacks, void *context);

// Dispatch one message that arrived on SEAMLESS_CHANNEL_ID. Returns 0 when the
// message was accepted, or a negative value when it was dropped.
int LiSeamlessHandleMessage(const uint8_t *payload, int length);

// Generic sender used by both ends.
int LiSendSeamlessMessage(const uint8_t *payload, int length);

// Client to host.
int LiSendWindowAck(uint8_t seq, uint8_t result);
int LiSendWindowInput(uint8_t windowId, uint8_t mode, uint32_t hostPid);
int LiSendWindowIntent(uint8_t windowId, uint8_t kind, int16_t x, int16_t y,
                       uint16_t width, uint16_t height);
int LiSendPresent(uint32_t frameId, uint32_t presentUs);
int LiSendSkip(uint32_t frameId, uint8_t reason);

// Host to client, encode only: fills caller storage and returns the length, so a
// host that owns its own transport can send the same bytes the senders below
// would send. No wire byte differs between these and the senders.
int LiEncodeWindowSnapshot(uint8_t *buffer, int capacity,
                           uint8_t seq, uint16_t canvasW, uint16_t canvasH,
                           int16_t workAreaX, int16_t workAreaY,
                           uint16_t workAreaW, uint16_t workAreaH,
                           const LiSeamlessWindowEntry *entries, int count);
int LiEncodeWindowDelta(uint8_t *buffer, int capacity,
                        uint8_t seq, uint8_t mask,
                        int16_t workAreaX, int16_t workAreaY,
                        uint16_t workAreaW, uint16_t workAreaH,
                        const LiSeamlessWindowEntry *entries, int count);

// Host to client.
int LiSendWindowSnapshot(uint8_t seq, uint16_t canvasW, uint16_t canvasH,
                         int16_t workAreaX, int16_t workAreaY,
                         uint16_t workAreaW, uint16_t workAreaH,
                         const LiSeamlessWindowEntry *entries, int count);
int LiSendWindowDelta(uint8_t seq, uint8_t mask,
                      int16_t workAreaX, int16_t workAreaY,
                      uint16_t workAreaW, uint16_t workAreaH,
                      const LiSeamlessWindowEntry *entries, int count);
int LiSendSeamlessIcon(uint32_t iconHash, const uint8_t *data, int length);
int LiSendCaretPosition(uint8_t windowId, int16_t x, int16_t y, uint16_t height);

#ifdef __cplusplus
}
#endif

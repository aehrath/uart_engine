/**
 * @file    ble_hci_parser.c
 * @brief   Thread-Safe BLE HCI H4 Transport Protocol Receiver Engine.
 *          Integrates directly into the FreeRTOS UART-DMA Consumer Thread.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "uart_dma/ble_hci_parser.h"
#include "uart_dma/uart_dma_messages.h"
#include "uart_dma/ble_hci_wire.h"

/** Incremental parser state. */
typedef enum {
    /** Waiting for an H4 packet indicator. */
    STATE_GET_PACKET_TYPE = 0,
    /** Collecting the packet-type-specific fixed header. */
    STATE_GET_HEADER,
    /** Collecting the declared variable payload. */
    STATE_GET_PAYLOAD,
    /** Complete packet is ready for callback dispatch. */
    STATE_PROCESS_FRAME
} HciParserState_t;

/** Dispatches one complete typed H4 packet. */
typedef void (*HciPacketDispatcher_t)(const BleHciHeader_t *header,
                                      const uint8_t *payload,
                                      uint16_t payloadLength);

/** Application dispatcher associated with one H4 packet indicator. */
typedef struct {
    /** Packet indicator matched on the wire. */
    uint8_t indicator;
    /** Completed-frame dispatcher. */
    HciPacketDispatcher_t dispatch;
} HciDispatchEntry_t;

/** Complete state retained while parsing a fragmented H4 stream. */
typedef struct {
    /** Current state-machine stage. */
    HciParserState_t state;
    /** Total fixed-header bytes expected for the active packet. */
    size_t headerExpectedLen;
    /** Optional application dispatcher selected by the indicator. */
    HciPacketDispatcher_t dispatch;
    /** Fixed header bytes collected so far. */
    uint16_t headerIdx;
    /** Variable payload size decoded from the fixed header. */
    uint16_t payloadExpectedLen;
    /** Variable payload bytes collected so far. */
    uint16_t payloadIdx;
    /** Packet-type-specific typed fixed-header storage. */
    BleHciHeader_t header;
    /** Variable packet payload storage. */
    uint8_t  payloadBuf[UART_DMA_MAX_PACKET_SIZE];
} BleHciContext_t;

/* Instantiate a localized instance for the consumer context */
static BleHciContext_t hciCtx = { .state = STATE_GET_PACKET_TYPE };

static BleHciEventHandler_t eventHandler = NULL;
static BleHciCommandHandler_t commandHandler = NULL;
static BleHciAclHandler_t aclHandler = NULL;

void BleHci_Reset(void) {
    memset(&hciCtx, 0, sizeof(hciCtx));
    hciCtx.state = STATE_GET_PACKET_TYPE;
}

void BleHci_SetEventHandler(BleHciEventHandler_t handler) {
    eventHandler = handler;
}

void BleHci_SetCommandHandler(BleHciCommandHandler_t handler) {
    commandHandler = handler;
}

void BleHci_SetAclHandler(BleHciAclHandler_t handler) {
    aclHandler = handler;
}

/** Dispatch a decoded Event packet when an application handler is installed. */
static void dispatchEvent(const BleHciHeader_t *header,
                          const uint8_t *payload, uint16_t payloadLength) {
    (void)payloadLength;
    if (eventHandler != NULL) {
        eventHandler(header->event.fields.event_code,
                     header->event.fields.parameter_length, payload);
    }
}

/** Dispatch a decoded Command packet when an application handler is installed. */
static void dispatchCommand(const BleHciHeader_t *header,
                            const uint8_t *payload, uint16_t payloadLength) {
    (void)payloadLength;
    if (commandHandler != NULL) {
        commandHandler(BleHciWire_FromLe16(header->command.fields.opcode_le),
                       header->command.fields.parameter_length, payload);
    }
}

/** Dispatch a decoded ACL packet when an application handler is installed. */
static void dispatchAcl(const BleHciHeader_t *header,
                        const uint8_t *payload, uint16_t payloadLength) {
    if (aclHandler != NULL) {
        aclHandler(BleHciWire_FromLe16(header->acl.fields.handle_flags_le),
                   payloadLength, payload);
    }
}

/** Packet types exposed through the current public callback API. */
static const HciDispatchEntry_t dispatchEntries[] = {
    {BLE_HCI_H4_COMMAND, dispatchCommand},
    {BLE_HCI_H4_ACL,     dispatchAcl},
    {BLE_HCI_H4_EVENT,   dispatchEvent},
};

/** Find an optional application dispatcher for an H4 packet indicator. */
static HciPacketDispatcher_t findDispatcher(uint8_t indicator) {
    for (size_t index = 0U;
         index < sizeof(dispatchEntries) / sizeof(dispatchEntries[0]);
         index++) {
        if (dispatchEntries[index].indicator == indicator) {
            return dispatchEntries[index].dispatch;
        }
    }
    return NULL;
}

/** @brief Feed one byte through the incremental H4 state machine.
 * @details DMA chunks may split
 * packet headers or payloads at arbitrary positions. */
void BleHci_ParseByte(uint8_t byte) {
    switch (hciCtx.state) {

        case STATE_GET_PACKET_TYPE:
            hciCtx.headerExpectedLen = BleHciWire_HeaderSize(byte);
            hciCtx.dispatch = findDispatcher(byte);
            hciCtx.header.bytes[0] = byte;
            hciCtx.headerIdx = sizeof(BleHciPacketIndicator_t);
            hciCtx.payloadIdx = 0;
            hciCtx.state = hciCtx.headerExpectedLen != 0U
                ? STATE_GET_HEADER : STATE_GET_PACKET_TYPE;
            break;

        case STATE_GET_HEADER:
            hciCtx.header.bytes[hciCtx.headerIdx++] = byte;
            if (hciCtx.headerIdx >= hciCtx.headerExpectedLen) {
                hciCtx.payloadExpectedLen = BleHciWire_PayloadLength(&hciCtx.header);

                /* Validate memory safety limits prior to accepting payload stream parsing */
                if (hciCtx.payloadExpectedLen > sizeof(hciCtx.payloadBuf) ||
                    hciCtx.payloadExpectedLen == 0U) {
                    if (hciCtx.payloadExpectedLen == 0) {
                        hciCtx.state = STATE_PROCESS_FRAME; /* Zero payload packet valid; parse immediately */
                    } else {
                        hciCtx.state = STATE_GET_PACKET_TYPE; /* Buffer constraint violation; reset state machine */
                    }
                } else {
                    hciCtx.state = STATE_GET_PAYLOAD;
                }
            }
            break;

        case STATE_GET_PAYLOAD:
            hciCtx.payloadBuf[hciCtx.payloadIdx++] = byte;
            if (hciCtx.payloadIdx >= hciCtx.payloadExpectedLen) {
                hciCtx.state = STATE_PROCESS_FRAME;
            }
            break;

        default:
            hciCtx.state = STATE_GET_PACKET_TYPE;
            break;
    }

    /* Terminal structural execution stage */
    if (hciCtx.state == STATE_PROCESS_FRAME) {
        if (hciCtx.dispatch != NULL) {
            hciCtx.dispatch(
                &hciCtx.header, hciCtx.payloadBuf, hciCtx.payloadExpectedLen);
        }

        /* Reset machine context to ingest the subsequent streaming packet sequence */
        hciCtx.state = STATE_GET_PACKET_TYPE;
        hciCtx.headerExpectedLen = 0U;
        hciCtx.dispatch = NULL;
    }
}

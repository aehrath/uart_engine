/** @file
 * @brief Implementation of native H4 formatting and stream parsing helpers. */
#include "h4_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t AppH4_Crc16(const void *data, size_t length) {
    const uint8_t *const bytes = data;
    uint16_t crc = 0xFFFFU;
    for (size_t index = 0U; index < length; index++) {
        crc ^= (uint16_t)bytes[index] << 8;
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x8000U) != 0U
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t AppH4_ParseHex(const char *text, uint8_t *output, size_t capacity) {
    size_t count = 0U;
    const char *cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',' || *cursor == ':') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            cursor += 2;
        }
        char *end = NULL;
        const unsigned long value = strtoul(cursor, &end, 16);
        if (end == cursor || value > 0xFFUL || count == capacity) {
            return SIZE_MAX;
        }
        output[count++] = (uint8_t)value;
        cursor = end;
    }
    return count;
}

void AppH4_FormatHex(char *destination, size_t capacity,
                     const void *data, size_t length) {
    const uint8_t *const bytes = data;
    size_t used = 0U;
    if (capacity == 0U) {
        return;
    }
    destination[0] = '\0';
    for (size_t index = 0U; index < length && used + 4U < capacity; index++) {
        const int written = snprintf(&destination[used], capacity - used,
                                     "%02X%s", bytes[index],
                                     index + 1U == length ? "" : " ");
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }
}

size_t AppH4Command_Init(BleHciCommandPacket_t *packet, uint16_t opcode,
                         const uint8_t *parameters, size_t parameterLength) {
    if (packet == NULL || parameterLength > BLE_HCI_MAX_COMMAND_PARAMETERS ||
        (parameterLength != 0U && parameters == NULL)) {
        return 0U;
    }
    packet->header = (BleHciCommandHeader_t){
        .indicator = {.value = BLE_HCI_H4_COMMAND},
        .fields = {
            .opcode_le = BleHciWire_ToLe16(opcode),
            .parameter_length = (uint8_t)parameterLength,
        },
    };
    if (parameterLength != 0U) {
        memcpy(packet->parameters, parameters, parameterLength);
    }
    return sizeof(packet->header) + parameterLength;
}

void AppH4StreamParser_Init(AppH4StreamParser_t *parser) {
    memset(parser, 0, sizeof(*parser));
}

/** Determine a complete CRC-protected frame size from an available H4 header.
 * @return Zero while incomplete, `SIZE_MAX` for an invalid indicator, or the
 * complete frame size. */
static size_t expectedFrameLength(const uint8_t *bytes, size_t available) {
    if (available < sizeof(BleHciPacketIndicator_t)) {
        return 0U;
    }
    const size_t headerLength = BleHciWire_HeaderSize(bytes[0]);
    if (headerLength == 0U) {
        return SIZE_MAX;
    }
    if (available < headerLength) {
        return 0U;
    }
    BleHciHeader_t header;
    memcpy(&header, bytes, headerLength);
    return headerLength + BleHciWire_PayloadLength(&header)
         + sizeof(BleHciCrcTrailer_t);
}

size_t AppH4StreamParser_Feed(AppH4StreamParser_t *parser,
                              const uint8_t *data, size_t length,
                              AppH4FrameHandler_t handler, void *context) {
    size_t validFrames = 0U;
    for (size_t index = 0U; index < length; index++) {
        if (parser->length == sizeof(parser->bytes)) {
            parser->length = 0U;
            parser->expected_length = 0U;
        }
        parser->bytes[parser->length++] = data[index];
        if (parser->expected_length == 0U) {
            const size_t expected = expectedFrameLength(parser->bytes, parser->length);
            if (expected == SIZE_MAX || expected > sizeof(parser->bytes)) {
                memmove(parser->bytes, &parser->bytes[1], --parser->length);
                continue;
            }
            parser->expected_length = expected;
        }
        if (parser->expected_length != 0U && parser->length == parser->expected_length) {
            const size_t h4Length = parser->length - sizeof(BleHciCrcTrailer_t);
            BleHciCrcTrailer_t trailer;
            memcpy(&trailer, &parser->bytes[h4Length], sizeof(trailer));
            const uint16_t received = BleHciWire_FromBe16(trailer.value_be);
            const bool valid = received == AppH4_Crc16(parser->bytes, h4Length);
            if (handler != NULL) {
                handler(parser->bytes, parser->length, valid, context);
            }
            validFrames += valid ? 1U : 0U;
            parser->length = 0U;
            parser->expected_length = 0U;
        }
    }
    return validFrames;
}

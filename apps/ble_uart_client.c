/** @file
 * @brief Interactive and one-shot native BLE HCI UART command-line client. */
#include "h4_wire.h"
#include "serial_port.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESPONSE_TIMEOUT_MS     2000

/** Named HCI command exposed by the terminal interface. */
typedef struct {
    /** Command-line token. */
    const char *name;
    /** HCI opcode in host byte order. */
    uint16_t opcode;
    /** Fixed preset parameter storage. */
    uint8_t parameters[8];
    /** Valid bytes in `parameters`. */
    uint8_t parameterLength;
} CommandPreset_t;

static const CommandPreset_t commandPresets[] = {
    {"reset",    0x0C03U, {0U},          0U},
    {"version",  0x1001U, {0U},          0U},
    {"address",  0x1009U, {0U},          0U},
    {"buffers",  0x2002U, {0U},          0U},
    {"scan-on",  0x200CU, {0x01U, 0x00U}, 2U},
    {"scan-off", 0x200CU, {0x00U, 0x00U}, 2U},
};

/** Print a byte span in uppercase hexadecimal. */
static void printHex(const void *data, size_t length) {
    const uint8_t *const bytes = data;
    for (size_t index = 0U; index < length; index++) {
        printf("%02X%s", bytes[index], index + 1U == length ? "" : " ");
    }
}

/** Transmit a typed H4 packet followed by its typed CRC trailer. */
static bool sendFramed(AppSerialPort_t *port, const void *h4Packet, size_t h4Length) {
    if (h4Packet == NULL || h4Length > BLE_HCI_MAX_WIRE_PACKET) {
        fputs("Packet is too large.\n", stderr);
        return false;
    }
    const uint16_t crc = AppH4_Crc16(h4Packet, h4Length);
    const BleHciCrcTrailer_t trailer = {BleHciWire_ToBe16(crc)};

    printf("TX   ");
    printHex(h4Packet, h4Length);
    putchar(' ');
    printHex(&trailer, sizeof(trailer));
    printf("  CRC %04X\n", crc);
    fflush(stdout);
    return AppSerialPort_Write(port, h4Packet, h4Length) &&
           AppSerialPort_Write(port, &trailer, sizeof(trailer));
}

/** Construct and transmit one HCI Command packet. */
static bool sendCommand(AppSerialPort_t *port, uint16_t opcode,
                        const uint8_t *parameters, size_t parameterLength) {
    BleHciCommandPacket_t command;
    const size_t commandLength = AppH4Command_Init(
        &command, opcode, parameters, parameterLength);
    if (commandLength == 0U) {
        fputs("HCI commands support at most 255 parameter bytes.\n", stderr);
        return false;
    }
    return sendFramed(port, &command, commandLength);
}

/** Print and decode a frame delivered by the incremental receive parser. */
static void describeFrame(const uint8_t *frame, size_t frameLength,
                          bool crcValid, void *context) {
    (void)context;
    const size_t h4Length = frameLength - 2U;
    printf("RX   ");
    printHex(frame, frameLength);
    printf("  %s", crcValid ? "CRC OK" : "CRC BAD");

    BleHciPacketIndicator_t indicator;
    memcpy(&indicator, frame, sizeof(indicator));
    if (indicator.value == APP_H4_EVENT && h4Length >= sizeof(BleHciEventHeader_t)) {
        BleHciEventHeader_t eventHeader;
        memcpy(&eventHeader, frame, sizeof(eventHeader));
        printf("; Event 0x%02X", eventHeader.fields.event_code);
        if (eventHeader.fields.event_code == 0x0EU &&
            h4Length >= sizeof(BleHciCommandCompleteEvent_t)) {
            BleHciCommandCompleteEvent_t event;
            memcpy(&event, frame, sizeof(event));
            printf(" Command Complete opcode=0x%04X status=%u",
                   BleHciWire_FromLe16(event.parameters.opcode_le),
                   event.parameters.status);
        }
    } else if (indicator.value == APP_H4_ACL &&
               h4Length >= sizeof(BleHciAclHeader_t)) {
        BleHciAclHeader_t header;
        memcpy(&header, frame, sizeof(header));
        printf("; ACL handle/flags=0x%04X",
               BleHciWire_FromLe16(header.fields.handle_flags_le));
    }
    putchar('\n');
    fflush(stdout);
}

/** Parse exactly one hexadecimal byte token. */
static bool parseHexByte(const char *text, uint8_t *value) {
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0' || parsed > 0xFFUL) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

/** Locate a command preset by its terminal token. */
static const CommandPreset_t *findPreset(const char *name) {
    for (size_t index = 0U; index < sizeof(commandPresets) / sizeof(commandPresets[0]); index++) {
        if (strcmp(commandPresets[index].name, name) == 0) {
            return &commandPresets[index];
        }
    }
    return NULL;
}

/** Print the interactive terminal command reference. */
static void printHelp(void) {
    puts("Commands:");
    puts("  reset | version | address | buffers | scan-on | scan-off");
    puts("  send OPCODE [PARAM ...]   custom HCI command in hexadecimal");
    puts("  raw BYTE [BYTE ...]       raw H4 packet; CRC is appended");
    puts("  help                       show this help");
    puts("  quit                       disconnect and exit");
}

/** Parse and execute one mutable terminal input line.
 * @return `false` when the caller should exit. */
static bool executeLine(AppSerialPort_t *port, char *line) {
    char *save = NULL;
    char *word = strtok_r(line, " \t\r\n", &save);
    if (word == NULL) {
        return true;
    }
    if (strcmp(word, "quit") == 0 || strcmp(word, "exit") == 0) {
        return false;
    }
    if (strcmp(word, "help") == 0) {
        printHelp();
        return true;
    }

    const CommandPreset_t *preset = findPreset(word);
    if (preset != NULL) {
        (void)sendCommand(port, preset->opcode, preset->parameters,
                          preset->parameterLength);
        return true;
    }

    if (strcmp(word, "send") == 0) {
        const char *opcodeText = strtok_r(NULL, " \t\r\n", &save);
        if (opcodeText == NULL) {
            fputs("Usage: send OPCODE [PARAM ...]\n", stderr);
            return true;
        }
        char *end = NULL;
        const unsigned long opcode = strtoul(opcodeText, &end, 16);
        if (*opcodeText == '\0' || *end != '\0' || opcode > 0xFFFFUL) {
            fputs("Invalid 16-bit hexadecimal opcode.\n", stderr);
            return true;
        }
        uint8_t parameters[0xFFU];
        size_t count = 0U;
        const char *parameter = NULL;
        while ((parameter = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            if (count == sizeof(parameters) || !parseHexByte(parameter, &parameters[count])) {
                fputs("Invalid parameter byte.\n", stderr);
                return true;
            }
            count++;
        }
        (void)sendCommand(port, (uint16_t)opcode, parameters, count);
        return true;
    }

    if (strcmp(word, "raw") == 0) {
        uint8_t packet[APP_H4_MAX_FRAME_SIZE - 2U];
        size_t count = 0U;
        const char *byteText = NULL;
        while ((byteText = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            if (count == sizeof(packet) || !parseHexByte(byteText, &packet[count])) {
                fputs("Invalid raw byte.\n", stderr);
                return true;
            }
            count++;
        }
        if (count == 0U) {
            fputs("Usage: raw BYTE [BYTE ...]\n", stderr);
        } else {
            (void)sendFramed(port, packet, count);
        }
        return true;
    }

    fprintf(stderr, "Unknown command '%s'. Type 'help'.\n", word);
    return true;
}

/** Send one preset and wait for a valid response frame. */
static int runOneShot(AppSerialPort_t *port, const char *commandName) {
    const CommandPreset_t *preset = findPreset(commandName);
    if (preset == NULL) {
        fprintf(stderr, "Unknown one-shot preset '%s'.\n", commandName);
        return 2;
    }
    if (!sendCommand(port, preset->opcode, preset->parameters, preset->parameterLength)) {
        return 1;
    }

    AppH4StreamParser_t parser;
    AppH4StreamParser_Init(&parser);
    struct pollfd serialInput = {AppSerialPort_Descriptor(port), POLLIN, 0};
    if (poll(&serialInput, 1U, RESPONSE_TIMEOUT_MS) <= 0) {
        fputs("Timed out waiting for a BLE response.\n", stderr);
        return 1;
    }
    uint8_t data[512];
    const int count = AppSerialPort_Read(port, data, sizeof(data));
    return count > 0 && AppH4StreamParser_Feed(
        &parser, data, (size_t)count, describeFrame, NULL) > 0U ? 0 : 1;
}

/** Poll standard input and the serial descriptor until the user exits. */
static int runInteractive(AppSerialPort_t *port) {
    AppH4StreamParser_t parser;
    AppH4StreamParser_Init(&parser);
    char line[2048];
    printHelp();
    fputs("ble-uart> ", stdout);
    fflush(stdout);

    bool running = true;
    while (running) {
        struct pollfd inputs[2] = {
            {STDIN_FILENO, POLLIN, 0},
            {AppSerialPort_Descriptor(port), POLLIN, 0}
        };
        const int result = poll(inputs, 2U, -1);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return 1;
        }
        if ((inputs[1].revents & POLLIN) != 0) {
            uint8_t data[512];
            const int count = AppSerialPort_Read(port, data, sizeof(data));
            if (count > 0) {
                (void)AppH4StreamParser_Feed(
                    &parser, data, (size_t)count, describeFrame, NULL);
                fputs("ble-uart> ", stdout);
                fflush(stdout);
            }
        }
        if ((inputs[0].revents & POLLIN) != 0) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                break;
            }
            running = executeLine(port, line);
            if (running) {
                fputs("ble-uart> ", stdout);
                fflush(stdout);
            }
        }
    }
    return 0;
}

/** Parse command-line options, open the port, and run the selected mode. */
int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: %s PORT [BAUD] [ONE_SHOT_PRESET]\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/ttys006 4000000 reset\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    const unsigned long baud = argc >= 3 ? strtoul(argv[2], &end, 10) : 4000000UL;
    if (argc >= 3 && (*argv[2] == '\0' || *end != '\0')) {
        fputs("Baud rate must be a decimal number.\n", stderr);
        return 2;
    }

    AppSerialPort_t port;
    AppSerialPort_Init(&port);
    char error[256];
    if (!AppSerialPort_Open(&port, argv[1], baud, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    printf("Connected to %s at %lu baud.\n", argv[1], baud);
    const int result = argc == 4 ? runOneShot(&port, argv[3]) : runInteractive(&port);
    AppSerialPort_Close(&port);
    return result;
}

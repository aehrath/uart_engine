#ifndef BLE_HCI_PARSER_H
#define BLE_HCI_PARSER_H

/** @file
 *  @brief Incremental Bluetooth HCI H4 packet parser API.
 *  @defgroup ble_hci_protocol Bluetooth HCI protocol
 *  @brief Packed wire layouts and incremental parsing for HCI H4 traffic.
 *  @{ */

#include <stdint.h>

/** Handles a completely decoded HCI Event packet.
 * @param event_code Bluetooth event code.
 * @param parameter_length Number of bytes in @p parameters.
 * @param parameters Event parameter bytes; valid only for the callback duration. */
typedef void (*BleHciEventHandler_t)(uint8_t event_code, uint8_t parameter_length,
                                     const uint8_t *parameters);
/** Handles a completely decoded HCI Command packet.
 * @param opcode Bluetooth command opcode in host byte order.
 * @param parameter_length Number of bytes in @p parameters.
 * @param parameters Command parameters; valid only for the callback duration. */
typedef void (*BleHciCommandHandler_t)(uint16_t opcode, uint8_t parameter_length,
                                       const uint8_t *parameters);
/** Handles a completely decoded HCI ACL Data packet.
 * @param handle_flags Connection handle and packet-boundary flags in host order.
 * @param data_length Number of bytes in @p data.
 * @param data ACL payload; valid only for the callback duration. */
typedef void (*BleHciAclHandler_t)(uint16_t handle_flags, uint16_t data_length,
                                   const uint8_t *data);

/** Feed one H4 byte into the global incremental parser. @param byte Next wire byte. */
void BleHci_ParseByte(uint8_t byte);
/** Discard a partial packet and return the parser to packet-type detection. */
void BleHci_Reset(void);
/** Install or clear the Event callback. @param handler Callback, or `NULL`. */
void BleHci_SetEventHandler(BleHciEventHandler_t handler);
/** Install or clear the Command callback. @param handler Callback, or `NULL`. */
void BleHci_SetCommandHandler(BleHciCommandHandler_t handler);
/** Install or clear the ACL callback. @param handler Callback, or `NULL`. */
void BleHci_SetAclHandler(BleHciAclHandler_t handler);

/** @} */

#endif

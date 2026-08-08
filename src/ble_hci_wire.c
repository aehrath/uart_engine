/** @file
 * @brief Operations on typed H4 packet headers. */

#include "uart_dma/ble_hci_wire.h"

size_t BleHciWire_HeaderSize(uint8_t indicator) {
    switch (indicator) {
        case BLE_HCI_H4_COMMAND: return sizeof(BleHciCommandHeader_t);
        case BLE_HCI_H4_ACL:     return sizeof(BleHciAclHeader_t);
        case BLE_HCI_H4_SCO:     return sizeof(BleHciScoHeader_t);
        case BLE_HCI_H4_EVENT:   return sizeof(BleHciEventHeader_t);
        case BLE_HCI_H4_ISO:     return sizeof(BleHciIsoHeader_t);
        default:                 return 0U;
    }
}

uint16_t BleHciWire_PayloadLength(const BleHciHeader_t *header) {
    switch (header->bytes[0]) {
        case BLE_HCI_H4_COMMAND:
            return header->command.fields.parameter_length;
        case BLE_HCI_H4_ACL:
            return BleHciWire_FromLe16(header->acl.fields.data_length_le);
        case BLE_HCI_H4_SCO:
            return header->sco.fields.data_length;
        case BLE_HCI_H4_EVENT:
            return header->event.fields.parameter_length;
        case BLE_HCI_H4_ISO:
            return BleHciWire_FromLe16(header->iso.fields.data_length_le) & 0x3FFFU;
        default:
            return 0U;
    }
}

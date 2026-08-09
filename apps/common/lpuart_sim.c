/** @file
 * @brief Deterministic NXP LPUART divisor and 8-N-1 receive sampler. */
#include "lpuart_sim.h"

#include <limits.h>
#include <string.h>

/** Return the ideal 8-N-1 line level at one bit within a packet. */
static uint8_t lineLevel(const uint8_t *bytes, size_t length,
                         uint64_t bitIndex) {
    const uint64_t byteIndex = bitIndex / 10U;
    const uint8_t bitInCharacter = (uint8_t)(bitIndex % 10U);
    if (byteIndex >= length || bitInCharacter == 9U) {
        return 1U;
    }
    if (bitInCharacter == 0U) {
        return 0U;
    }
    return (uint8_t)((bytes[byteIndex] >> (bitInCharacter - 1U)) & 1U);
}

bool AppLpuartSim_Configure(AppLpuartSimulator_t *simulator,
                            uint32_t sourceClockHz, uint32_t requestedBaud) {
    if (simulator == NULL || sourceClockHz == 0U || requestedBaud == 0U ||
        sourceClockHz > APP_LPUART_MAX_SOURCE_CLOCK_HZ ||
        requestedBaud > APP_LPUART_MAX_REQUESTED_BAUD) {
        return false;
    }

    uint64_t bestError = UINT64_MAX;
    uint8_t bestOsr = APP_LPUART_MIN_OSR;
    uint16_t bestSbr = 1U;
    for (uint8_t osr = APP_LPUART_MIN_OSR; osr <= APP_LPUART_MAX_OSR; osr++) {
        for (uint16_t sbr = 1U; sbr <= APP_LPUART_MAX_SBR; sbr++) {
            const uint64_t divisor = (uint64_t)osr * sbr;
            const uint64_t target = (uint64_t)requestedBaud * divisor;
            const uint64_t error = sourceClockHz >= target
                ? (uint64_t)sourceClockHz - target : target - sourceClockHz;
            /* Compare absolute baud error without rounding or floating point. */
            const uint64_t scaledError = error * (uint64_t)bestOsr * bestSbr;
            const uint64_t scaledBest = bestError == UINT64_MAX
                ? UINT64_MAX : bestError * divisor;
            if (scaledError < scaledBest ||
                (scaledError == scaledBest && osr > bestOsr)) {
                bestError = error;
                bestOsr = osr;
                bestSbr = sbr;
            }
        }
    }

    const uint64_t divisor = (uint64_t)bestOsr * bestSbr;
    simulator->sourceClockHz = sourceClockHz;
    simulator->requestedBaud = requestedBaud;
    simulator->osr = bestOsr;
    simulator->sbr = bestSbr;
    simulator->actualBaud = (uint32_t)(((uint64_t)sourceClockHz + divisor / 2U) /
                                       divisor);
    const int64_t difference = (int64_t)sourceClockHz -
        (int64_t)((uint64_t)requestedBaud * divisor);
    simulator->errorPpm = (int32_t)((difference * 1000000LL) /
        (int64_t)((uint64_t)requestedBaud * divisor));
    simulator->phase = 0U;
    return true;
}

AppLpuartSimulationResult_t AppLpuartSim_Receive(
    AppLpuartSimulator_t *simulator, const uint8_t *input,
    uint8_t *output, size_t length) {
    AppLpuartSimulationResult_t result = {0U, 0U, true};
    if (simulator == NULL || input == NULL || output == NULL ||
        simulator->sourceClockHz == 0U || simulator->requestedBaud == 0U) {
        result.valid = false;
        return result;
    }

    const uint64_t denominator =
        (uint64_t)simulator->requestedBaud * simulator->sbr;
    const uint64_t phaseStep =
        ((uint64_t)10U * simulator->sourceClockHz) % denominator;
    for (size_t byteIndex = 0U; byteIndex < length; byteIndex++) {
        const uint64_t detectionDelay = simulator->phase == 0U
            ? 0U : denominator - simulator->phase;
        uint8_t sampled = 0U;
        for (uint8_t dataBit = 0U; dataBit < 8U; dataBit++) {
            const uint64_t sampleOffset =
                ((uint64_t)(3U + 2U * dataBit) * simulator->osr + 1U) / 2U;
            const uint64_t positionNumerator = detectionDelay +
                sampleOffset * denominator;
            const uint64_t relativeBit = positionNumerator /
                simulator->sourceClockHz;
            sampled |= (uint8_t)(lineLevel(input, length,
                (uint64_t)byteIndex * 10U + relativeBit) << dataBit);
        }
        const uint64_t stopOffset =
            ((uint64_t)19U * simulator->osr + 1U) / 2U;
        const uint64_t stopBit = (detectionDelay + stopOffset * denominator) /
            simulator->sourceClockHz;
        if (lineLevel(input, length, (uint64_t)byteIndex * 10U + stopBit) == 0U) {
            result.framingErrors++;
        }
        output[byteIndex] = sampled;
        if (sampled != input[byteIndex]) {
            result.changedBytes++;
        }
        simulator->phase = (simulator->phase + phaseStep) % denominator;
    }
    result.valid = result.changedBytes == 0U && result.framingErrors == 0U;
    return result;
}

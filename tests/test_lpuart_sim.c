/** @file
 * @brief Native assertions for KW47 LPUART divisor and sampling simulation. */
#include "lpuart_sim.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Verify exact 20 MHz divisor solutions and a clock-starved packet failure. */
int main(void) {
    static const uint8_t packet[] = {
        0x01U, 0x03U, 0x0CU, 0x00U, 0x3EU, 0xA5U, 0x5AU, 0xFFU,
    };
    uint8_t sampled[sizeof(packet)];
    AppLpuartSimulator_t simulator;

    assert(AppLpuartSim_Configure(&simulator, 20000000U, 4000000U));
    assert(simulator.osr == 5U);
    assert(simulator.sbr == 1U);
    assert(simulator.actualBaud == 4000000U);
    AppLpuartSimulationResult_t result = AppLpuartSim_Receive(
        &simulator, packet, sampled, sizeof(packet));
    assert(result.valid);
    assert(memcmp(packet, sampled, sizeof(packet)) == 0);

    assert(AppLpuartSim_Configure(&simulator, 20000000U, 5000000U));
    assert(simulator.osr == 4U);
    assert(simulator.sbr == 1U);
    assert(simulator.actualBaud == 5000000U);

    /* clock / 4 is below 4 Mbaud, but the sampled short packet still survives;
     * callers must therefore warn from this result rather than the ceiling. */
    assert(AppLpuartSim_Configure(&simulator, 15900000U, 4000000U));
    result = AppLpuartSim_Receive(&simulator, packet, sampled, sizeof(packet));
    assert(result.valid);

    assert(AppLpuartSim_Configure(&simulator, 12000000U, 4000000U));
    assert(simulator.osr == 4U);
    assert(simulator.sbr == 1U);
    assert(simulator.actualBaud == 3000000U);
    result = AppLpuartSim_Receive(&simulator, packet, sampled, sizeof(packet));
    assert(!result.valid);
    assert(result.changedBytes != 0U || result.framingErrors != 0U);

    puts("All LPUART timing simulation tests passed.");
    return 0;
}

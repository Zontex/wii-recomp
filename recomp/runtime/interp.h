// Runs guest code that has no translation.
#pragma once
#include <stdint.h>

// Runs from an address until the code returns to its caller. Stops the program if the
// address can't hold code.
void interpret(uint32_t address);
// Runs one instruction that isn't a branch.
void interpret_instruction(uint32_t address, uint32_t word);

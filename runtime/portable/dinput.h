/* DirectInput 8 over SDL3 input (dinput.c, input.c). */
#pragma once

/* Registers the shims (before the images are mapped). */
void dinput_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void dinput_setup(void);

#include "input.h"
/* Set by the addon host: sees each XInput read the game makes (pad `user`) and may change it (a
 * button an addon took stays up for the game). */
extern void (*dinput_xpad_hook)(uint32_t user, XPad* pad);

/* DirectSound 8 on SDL3 audio (dsound.c). */
#pragma once

/* Registers the shims (before the images are mapped). */
void dsound_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void dsound_setup(void);

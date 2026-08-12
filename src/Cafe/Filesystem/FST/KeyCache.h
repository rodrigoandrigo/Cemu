#pragma once

void KeyCache_Prepare();
// Discards the process-wide cache and reloads keys.txt. Call only while no
// title is being mounted. Returns the number of valid AES-128 keys loaded.
uint32 KeyCache_Reload();

uint8* KeyCache_GetAES128(sint32 index);

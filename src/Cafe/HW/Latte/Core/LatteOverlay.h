#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);
// Session-only override used by embedded hosts. It does not modify settings.xml.
void LatteOverlay_setHostPerformanceMetrics(bool enabled);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);

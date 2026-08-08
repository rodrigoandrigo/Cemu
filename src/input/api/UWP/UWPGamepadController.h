#pragma once

#include "input/api/Controller.h"

// Input state supplied by an embedding UWP/Xbox host.  Keeping this adapter
// free of WinRT types is important: Windows.Gaming.Input objects are
// apartment-affine on Xbox, while Cemu's input threads are not.
class UWPGamepadController final : public ControllerBase
{
public:
	UWPGamepadController();

	static void SetHostState(bool connected, uint32 buttons,
		float leftX, float leftY, float rightX, float rightY,
		float leftTrigger, float rightTrigger);
	static bool IsHostGamepadConnected();

	std::string_view api_name() const override { return "Windows.Gaming.Input"; }
	InputAPI::Type api() const override { return InputAPI::WGIGamepad; }
	bool is_connected() override;
	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;
};

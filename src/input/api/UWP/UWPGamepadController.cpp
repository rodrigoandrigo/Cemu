#include "input/api/UWP/UWPGamepadController.h"

#include <algorithm>
#include <mutex>

namespace
{
struct HostGamepadState
{
	bool connected = false;
	uint32 buttons = 0;
	float leftX = 0.0f;
	float leftY = 0.0f;
	float rightX = 0.0f;
	float rightY = 0.0f;
	float leftTrigger = 0.0f;
	float rightTrigger = 0.0f;
};

std::mutex s_hostGamepadMutex;
HostGamepadState s_hostGamepadState;

float ClampAxis(float value)
{
	return (std::max)(-1.0f, (std::min)(value, 1.0f));
}
}

UWPGamepadController::UWPGamepadController()
	: ControllerBase("host-wgi-gamepad", "Xbox Gamepad")
{
}

void UWPGamepadController::SetHostState(bool connected, uint32 buttons,
	float leftX, float leftY, float rightX, float rightY,
	float leftTrigger, float rightTrigger)
{
	std::scoped_lock lock(s_hostGamepadMutex);
	s_hostGamepadState = {
		connected,
		buttons,
		ClampAxis(leftX), ClampAxis(leftY),
		ClampAxis(rightX), ClampAxis(rightY),
		(std::max)(0.0f, (std::min)(leftTrigger, 1.0f)),
		(std::max)(0.0f, (std::min)(rightTrigger, 1.0f))
	};
}

bool UWPGamepadController::IsHostGamepadConnected()
{
	std::scoped_lock lock(s_hostGamepadMutex);
	return s_hostGamepadState.connected;
}

bool UWPGamepadController::is_connected()
{
	return IsHostGamepadConnected();
}

std::string UWPGamepadController::get_button_name(uint64 button) const
{
	return ControllerBase::get_button_name(button);
}

ControllerState UWPGamepadController::raw_state()
{
	HostGamepadState state;
	{
		std::scoped_lock lock(s_hostGamepadMutex);
		state = s_hostGamepadState;
	}

	ControllerState result{};
	if (!state.connected)
		return result;

	for (uint32 button = 0; button < 16; ++button)
	{
		if ((state.buttons & (1u << button)) != 0)
			result.buttons.SetButtonState(button, true);
	}
	result.axis = { state.leftX, state.leftY };
	result.rotation = { state.rightX, state.rightY };
	result.trigger = { state.leftTrigger, state.rightTrigger };
	return result;
}

#include "input/api/SDL/SDLControllerProvider.h"

#include "input/api/SDL/SDLController.h"
#include "util/helpers/TempState.h"

#include <SDL3/SDL.h>
#include <boost/functional/hash.hpp>

#if defined(CEMU_UWP)
#include <roapi.h>
#endif

struct SDL_JoystickGUIDHash
{
	std::size_t operator()(const SDL_GUID& guid) const
	{
		return boost::hash_value(guid.data);
	}
};

SDLControllerProvider::SDLControllerProvider()
{
#if !BOOST_OS_MACOS
	std::scoped_lock _l(s_mutex);
	if (s_initCount.fetch_add(1) == 0)
	{
		s_running = true;
		s_thread = std::thread(&SDLControllerProvider::event_thread, this);
	}
#endif
}

SDLControllerProvider::~SDLControllerProvider()
{
#if !BOOST_OS_MACOS
	bool shutdownSDL = false;
	{
		std::scoped_lock _l(s_mutex);
		if (s_initCount.fetch_sub(1) == 1)
		{
			cemu_assert_debug(s_running);
			s_running = false;
			shutdownSDL = true;
		}
	}

	if (shutdownSDL)
	{
		// wake the thread with a quit event if it's currently waiting for events
		SDL_Event evt;
		SDL_zero(evt);
		evt.type = SDL_EVENT_QUIT;
		SDL_PushEvent(&evt);
		if (s_thread.joinable())
		{
			s_thread.join();
		}
	}
#endif
}

std::vector<std::shared_ptr<ControllerBase>> SDLControllerProvider::get_controllers()
{
	std::vector<std::shared_ptr<ControllerBase>> result;

	std::unordered_map<SDL_GUID, size_t, SDL_JoystickGUIDHash> guid_counter;

	TempState lock(SDL_LockJoysticks, SDL_UnlockJoysticks);
	int gamepad_count = 0;
	SDL_JoystickID *gamepad_ids = SDL_GetGamepads(&gamepad_count);
	if (gamepad_ids)
	{
		for (size_t i = 0; i < gamepad_count; ++i)
		{
			const auto guid = SDL_GetGamepadGUIDForID(gamepad_ids[i]);
			const auto it = guid_counter.try_emplace(guid, 0);
			if (const char* name = SDL_GetGamepadNameForID(gamepad_ids[i]))
				result.emplace_back(std::make_shared<SDLController>(guid, it.first->second, name));
			else
				result.emplace_back(std::make_shared<SDLController>(guid, it.first->second));
			++it.first->second;
		}
		SDL_free(gamepad_ids);
	}
	return result;
}

int SDLControllerProvider::get_index(size_t guid_index, const SDL_GUID& guid) const
{
	size_t index = 0;
	int gamepad_count = 0;
	TempState lock(SDL_LockJoysticks, SDL_UnlockJoysticks);
	SDL_JoystickID *gamepad_ids = SDL_GetGamepads(&gamepad_count);
	if (gamepad_ids)
	{
		for (size_t i = 0; i < gamepad_count; ++i)
		{
			if (guid == SDL_GetGamepadGUIDForID(gamepad_ids[i]))
			{
				if (index == guid_index)
				{
					SDL_free(gamepad_ids);
					return i;
				}
				++index;
			}
		}
		SDL_free(gamepad_ids);
	}
	return -1;
}

MotionSample SDLControllerProvider::motion_sample(SDL_JoystickID diid)
{
	std::shared_lock lock(s_mutex);
	auto it = s_motion_states.find(diid);
	return (it != s_motion_states.end()) ? it->second.data : MotionSample{};
}

void SDLControllerProvider::InitSDL()
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
#if defined(CEMU_UWP)
	// Windows.Gaming.Input is the controller transport available to a packaged
	// UWP application. SDL deliberately leaves this backend disabled by
	// default, so the XAML host could see an Xbox controller while
	// SDL_GetGamepads() (and therefore Cemu) saw no devices at all.
	// This hint must be set before SDL_INIT_GAMEPAD is initialized.
	SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
#endif
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STADIA, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_LUNA, "1");

	Uint32 subsystemFlags = SDL_INIT_GAMEPAD;
#if !defined(CEMU_UWP)
	subsystemFlags |= SDL_INIT_HAPTIC;
#endif
	if (!SDL_InitSubSystem(subsystemFlags))
	{
		throw std::runtime_error(fmt::format("couldn't initialize SDL: {}", SDL_GetError()));
	}

	SDL_SetGamepadEventsEnabled(true);
	if (!SDL_GamepadEventsEnabled())
	{
		cemuLog_log(LogType::Force, "Couldn't enable SDL gamecontroller event polling: {}", SDL_GetError());
	}

#if defined(CEMU_UWP)
	int gamepadCount = 0;
	SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepadCount);
	SDL_free(gamepads);
	cemuLog_log(LogType::Force,
		"UWP controller backend initialized through Windows.Gaming.Input ({} gamepad{})",
		gamepadCount, gamepadCount == 1 ? "" : "s");
#endif
}

void SDLControllerProvider::QueueRumble(SDL_JoystickID instanceId, Uint16 lowFrequency, Uint16 highFrequency)
{
	if (instanceId < 0)
		return;
	std::scoped_lock lock(s_rumbleMutex);
	s_pendingRumble[instanceId] = { lowFrequency, highFrequency };
}

void SDLControllerProvider::ApplyPendingRumble()
{
	std::unordered_map<SDL_JoystickID, std::pair<Uint16, Uint16>> pending;
	{
		std::scoped_lock lock(s_rumbleMutex);
		pending.swap(s_pendingRumble);
	}

	for (const auto& [instanceId, strength] : pending)
	{
		if (SDL_Gamepad* gamepad = SDL_GetGamepadFromID(instanceId))
		{
			// Wii U patterns are refreshed continuously.  Keep a finite timeout
			// so a suspended/removed controller cannot remain vibrating.
			SDL_RumbleGamepad(gamepad, strength.first, strength.second,
				(strength.first || strength.second) ? 250 : 0);
		}
	}
}

void SDLControllerProvider::ShutdownSDL()
{
	Uint32 subsystemFlags = SDL_INIT_GAMEPAD;
#if !defined(CEMU_UWP)
	subsystemFlags |= SDL_INIT_HAPTIC;
#endif
	SDL_QuitSubSystem(subsystemFlags);
}

#if BOOST_OS_MACOS
void SDLControllerProvider::PumpSDLEvents()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
		HandleSDLEvent(event);
}
#endif

void SDLControllerProvider::HandleSDLEvent(SDL_Event& event)
{
	switch (event.type)
	{
		case SDL_EVENT_QUIT:
		{
			std::scoped_lock _l(s_mutex);
			s_running = false;
			break;
		}
		case SDL_EVENT_GAMEPAD_AXIS_MOTION: /**< Game controller axis motion */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN: /**< Game controller button pressed */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_BUTTON_UP: /**< Game controller button released */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_ADDED: /**< A new Game controller has been inserted into the system */
		{
			std::scoped_lock _l(s_mutex);
			InputManager::instance().on_device_changed();
			break;
		}
		case SDL_EVENT_GAMEPAD_REMOVED: /**< An opened Game controller has been removed */
		{
			{
				std::scoped_lock rumbleLock(s_rumbleMutex);
				s_pendingRumble.erase(event.gdevice.which);
			}
			std::scoped_lock _l(s_mutex);
			InputManager::instance().on_device_changed();
			s_motion_states.erase(event.gdevice.which);
			break;
		}
		case SDL_EVENT_GAMEPAD_REMAPPED: 			/**< The controller mapping was updated */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:		/**< Game controller touchpad was touched */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:		/**< Game controller touchpad finger was moved */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:			/**< Game controller touchpad finger was lifted */
		{
			break;
		}
		case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:		/**< Game controller sensor was updated */
		{
			SDL_JoystickID id = event.gsensor.which;
			uint64_t ts = event.gsensor.timestamp;
			std::scoped_lock _l(s_mutex);
			auto& state = s_motion_states[id];
			auto& tracking = state.tracking;

			if (event.gsensor.sensor == SDL_SENSOR_ACCEL)
			{
				const auto dif = ts - tracking.lastTimestampAccel;
				if (dif <= 0)
				{
					break;
				}

				if (dif >= 10000000000)
				{
					tracking.hasAcc = false;
					tracking.hasGyro = false;
					tracking.lastTimestampAccel = ts;
					break;
				}

				tracking.lastTimestampAccel = ts;
				tracking.acc[0] = -event.gsensor.data[0] / 9.81f;
				tracking.acc[1] = -event.gsensor.data[1] / 9.81f;
				tracking.acc[2] = -event.gsensor.data[2] / 9.81f;
				tracking.hasAcc = true;
			}
			if (event.gsensor.sensor == SDL_SENSOR_GYRO)
			{
				const auto dif = ts - tracking.lastTimestampGyro;
				if (dif <= 0)
				{
					break;
				}

				if (dif >= 10000000000)
				{
					tracking.hasAcc = false;
					tracking.hasGyro = false;
					tracking.lastTimestampGyro = ts;
					break;
				}

				tracking.lastTimestampGyro = ts;
				tracking.gyro[0] = event.gsensor.data[0];
				tracking.gyro[1] = -event.gsensor.data[1];
				tracking.gyro[2] = -event.gsensor.data[2];
				tracking.hasGyro = true;
			}
			if (tracking.hasAcc && tracking.hasGyro)
			{
				auto ts = std::max(tracking.lastTimestampGyro, tracking.lastTimestampAccel);

				if (ts > tracking.lastTimestampIntegrate)
				{
					const auto tsDif = ts - tracking.lastTimestampIntegrate;
					tracking.lastTimestampIntegrate = ts;
					float tsDifD = (float)tsDif / 1000000000.0f;

					if (tsDifD >= 1.0f)
					{
						tsDifD = 1.0f;
					}

					state.handler.processMotionSample(tsDifD, tracking.gyro.x, tracking.gyro.y, tracking.gyro.z, tracking.acc.x, -tracking.acc.y, -tracking.acc.z);
					state.data = state.handler.getMotionSample();
				}

				tracking.hasAcc = false;
				tracking.hasGyro = false;
			}
			break;
		}
	}
}

void SDLControllerProvider::event_thread()
{
#if BOOST_OS_MACOS
	cemu_assert(false);
#endif
	SetThreadName("SDL_events");
	try
	{
#if defined(CEMU_UWP)
		// SDL's WGI objects are created and updated on this worker.  A std::thread
		// does not inherit the XAML thread's COM apartment, which is especially
		// important when the package runs as a game on Xbox.  Keep every
		// GetCurrentReading call on this initialized MTA instead of invoking
		// SDL_UpdateGamepads from Cemu's input/PPC threads.
		const HRESULT apartmentResult = RoInitialize(RO_INIT_MULTITHREADED);
		const bool uninitializeApartment = SUCCEEDED(apartmentResult);
		if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE)
			throw std::runtime_error(fmt::format(
				"couldn't initialize the UWP controller COM apartment: 0x{:08X}",
				static_cast<uint32_t>(apartmentResult)));
#endif
		InitSDL();
		while (s_running.load(std::memory_order_relaxed))
		{
#if defined(CEMU_UWP)
			// WGI is a polling backend.  SDL_WaitEvent alone only guarantees
			// arrival/removal delivery and used to leave polling to raw_state(),
			// where it raced the event thread and crossed COM apartments.  A 4 ms
			// cadence is fast enough for an Xbox controller while keeping one
			// unambiguous owner for the WGI runtime objects.
			SDL_UpdateGamepads();
			SDL_Event event{};
			while (SDL_PollEvent(&event))
			{
				HandleSDLEvent(event);
				if (!s_running.load(std::memory_order_relaxed))
					break;
			}
			// Process removal events before resolving an instance ID for rumble.
			ApplyPendingRumble();
			SDL_Delay(4);
#else
			SDL_Event event{};
			SDL_WaitEvent(&event);
			HandleSDLEvent(event);
#endif
		}
		ShutdownSDL();
#if defined(CEMU_UWP)
		if (uninitializeApartment)
			RoUninitialize();
#endif
	}
	catch (const std::exception& error)
	{
		s_running.store(false, std::memory_order_relaxed);
		cemuLog_log(LogType::Force, "SDL gamepad initialization failed: {}", error.what());
	}
	catch (...)
	{
		s_running.store(false, std::memory_order_relaxed);
		cemuLog_log(LogType::Force, "SDL gamepad initialization failed with an unknown error");
	}
}

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "RtMidi.h"

static constexpr size_t MaxIncomingMidiMessages = 8192;
static constexpr size_t MaxMonitorMidiMessages = 2048;

// fwd dec for InputItem's gluemidi pointer type
class GlueMidiWorker;

struct QueuedMidiMessage
{
	uint64_t InputId = 0;
	double DeltaTime = 0.0;
	std::vector<unsigned char> Data;
	bool ShouldRoute = false;
};

struct InputItem {

	GlueMidiWorker* Worker = nullptr; // The main GM instance	
	std::unique_ptr<RtMidiIn> midiinput; // To hold the MidiIn object that we'll create
	std::string NameIndexed; // Name suffixed by the enumeration index (unreliable)
	std::string Name; // Name without index suffix, for comparison purposes (slightly less unreliable)
	std::atomic<bool> Active_atomic{ false };
	std::atomic<bool> Muted_atomic{ false }; // For passthrough filtering purposes
	bool LogMute;
	unsigned int Index; // This input's index as of the last port enumeration (relibable only until the next enumeration)
	uint64_t Id;
	std::atomic<bool> AcceptCallbacks_atomic{ false };

	// On startup/refresh enumeration, we loop all available inputs and create an
	// InputItem for each. It has name, index and a pointer to the gm instance so
	// it can be looped for UI display, but it has no RtMidiIn object until that's
	// set up in openMidiInPort.
	InputItem(GlueMidiWorker* InWorker, std::string InName, unsigned int InIndex)
		: Worker(InWorker), NameIndexed(std::move(InName)), Index(InIndex)
	{
		const size_t LastSpace = NameIndexed.find_last_of(' ');
		Name = LastSpace == std::string::npos
			? NameIndexed
			: NameIndexed.substr(0, LastSpace);
		LogMute = false;		
		Id = 0;
	};

};

// UI-facing enumeration/etc stuff
struct MidiInputUiState
{
	uint64_t Id = 0;
	int Index = -1;
	std::string Name;

	bool Active = false;
	bool Muted = false;
	bool LogMute = false;
};

struct MidiPortUiSnapshot
{
	std::vector<MidiInputUiState> Inputs;
	std::vector<std::string> Outputs;

	int CurrentOutputIndex = -1;
	bool OutputOpen = false;
};

enum class EMidiCommandType
{
	OpenOutput,
	CloseOutput,
	OpenInput,
	CloseInput,
	RefreshPorts,
	ReleaseAll,
	SetInputMuted,
	CheckPorts,
	Shutdown
};

struct MidiCommand
{
	EMidiCommandType Type;
	int OutputIndex = -1;
	int InputIndex = -1;
	bool BoolValue = false; // used for Muted etc
};

struct InputState
{
	std::atomic<bool> Open{ false };
};

// Used to snapshot the mappings before a 
// refresh so a restore can be attempted immediately after
struct InputRestoreState
{
	std::string NameIndexed;
	bool Muted = false;
};
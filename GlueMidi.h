#pragma once
#include "GlueMidiStructs.h"
#include "GlueMidiThemes.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#define LINEBUFFERMAX 1023
#define LINESMAX 1023

// Forward declare for the struct 
class GlueMidi;
class GlueMidiWorker;

class GlueMidi {

public:
	// GlueMidi instance is constructed with a pointer to a function in 
	// main.cpp that will be our icon animation update call
	GlueMidi(void (*func)());

	~GlueMidi();

	void CallAnimate()
	{
		// Call the animation callback that was passed on construction
		callbackFunc();
	}

private:
	void (*callbackFunc)(); // Function pointer member

public:

	// 2026-08-15 Working towards a v0.2.1 that's more robust against 
	// shitty device drivers. All these mutex locks are temporary
	// until I a) get all the dodgy RT-touching stuff out of the UI 
	// loop and b) eventually put the worker thread and RT ownership
	// into a separate process.
	// 
	// 2026-08-16 Lots of improvements, pretty much full separation
	// between UI and worker thread. Next step is isolating the worker
	// thread stuff as a separate process, which means the UI can 
	// catch some faults (and report them), kill and restart the worker
	// (hopefully, sometimes), and hopefully prevent orphaned processes.
	//
	// On the other hand...it can't help fuckups in kernel-mode driver
	// state that might still leave the worker thread/process unkillable
	// and require a system reboot before MIDI (or at least GlueMidi) is
	// usable again. But in that situation, GlueMidi's not the only app
	// that's struggling and you probably need a reboot anyway.
	//
	// So I'll not embark on it just yet. I'll stress-test these latest
	// improvements first.

	std::unique_ptr<GlueMidiWorker> MidiWorker_;
	
	void ProcessMidiMessageForMonitor(const MidiInputUiState& Input, double deltatime, const std::vector<unsigned char>& message);

	void ProcessMonitorMidiMessages();
	
	std::mutex midiLogMutex;

	bool bDone;

	int width;
	int height;

	// Keep this updated at runtime for saving on close
	int wposx;
	int wposy;

	// loaded ini settings
	bool settings_were_loaded = false;
	std::string iniFileName;
	std::unordered_map<std::string, std::string> settings_pairs;

	// for restoring extant *intent* after an enumeration, so that 
	// temporarily unplugged devices can be displayed in red text
	// despite being disabled, then revived when rthey're plugged in
	void ReconcileDesiredMidiState(const MidiPortUiSnapshot& Snapshot);
	
	std::unordered_set<std::string> PendingInputOpenNames;
	std::string PendingOutputOpenName;

	// possibly redundant now?
	std::mutex SettingsMutex;

	// Optionally sets this option to "" if it's not found
	std::string GetConfigString(const std::string& Key, bool CreateIfNotFound = false);
	
	// Optionally sets this option to -1 if it's not found
	int GetConfigInt(const std::string& Key, bool CreateIfNotFound = false);
	
	// Deserialise a stored string array from the ini, creating an empty key if not found
	std::vector<std::string> GetConfigStringArray(const std::string& Key, bool CreateIfNotFound = false);

	void SetConfigString(const std::string& Key, const std::string& Value);
	void SetConfigInt(const std::string& Key, const int Value);
	void SetConfigStringArray(const std::string& Key, const std::vector<std::string>& Values);

	// ImGui globals
	char inputStatus[32] = "Inactive";
	char outputStatus[32] = "Inactive";

	float baseFontSize = 13.0f;
	float iconFontSize = baseFontSize;

	bool isDragging = false;
	bool isMenuHovered = false;
	bool dragStartedOnTitlebar = false;
	ImVec2 dragStartPos;

	ImFont* iconFont;

	// Only the worker thread should touch these
	unsigned int InPortCount = 0;
	unsigned int OutPortCount = 0;

	std::vector<std::unique_ptr<InputItem>> InputItems;
	
	std::vector<std::string> MidiOutNames;
	
	std::vector<std::string> MidiLogs;

    std::vector<unsigned char> IncomingMidiMessage;
    int MidiInNBytes;
    double stamp;

    // Cache previous channel and CC num to detect 14-bit messages
    int lastChannel;
    int lastCCnum;
    int lastCCvalue;


	// Filter incoming messages by	
	char buf_filter[32] = "";
	int filterChannel = 0; // show only this channel (0 disables)
	int filterCC = -1; // show only this CC number (-1 disables)
	bool filter14bit= false; // show only 14bit values 
	bool displayRaw = false; // show only raw bytes

	bool filterShowNotes = true;
	bool filterShowCC = true;
	bool filterShowSys = true;
	bool filterShowSysex = true;
	bool filterShowRaw = true;
	bool filterShowOther = true;// program change, pitchbend, aftertouch etc

	double AnimDeltaCounter = 0;
	double AnimDeltaThreshold = 0.100;

	// Main ImGui draw window
	void Update();

	// File IO for settings ini
	std::string getExecutableDirectory();

	void writeAppSettings(const std::string& fileName, const std::unordered_map<std::string, std::string>& keyValuePairs);

	bool readAppSettings(const std::string& fileName, std::unordered_map<std::string, std::string>& keyValuePairs);

	void setupImGuiFonts();

	// Flush out and renew our lists of discovered input and output MIDI ports
	int refreshMidiPorts();

	void releaseMidiPorts();

    // Add a log line to the log output text view
    void Log(const char* fmt);

	// Call after enabling or disabling an input. The config array should contain
	// every name of an input we've ever wanted to have enabled, regardless of 
	// whether it's active (it might just be physically unplugged). We only remove
	// it from the list if it's enumerated and then explicitly disabled from the UI.
	void UpdateInputsConfig(std::string Name, bool bRemove = false);

	void SaveSettings();

	// Set this in UI when doing a Release All or deliberately deselecting
	// an output, or else everything'll get forcibly reenabled from settings.
	bool bSuppressDesiredStateReconcile = false;

};


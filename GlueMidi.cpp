#include "GlueMidi.h"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include "IconsFontAwesome6.h"
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <windows.h>
#define __WINDOWS_MM__
#include "RtMidi.h"

template <typename T>
void remove(std::vector<T>& vec, std::size_t pos)
{
	typename std::vector<T>::iterator it = vec.begin();
	std::advance(it, pos);
	vec.erase(it);
}

// Helper function to transform a string to lowercase and remove whitespace
std::string normaliseString(const std::string& str) {
	std::string result;
	for (char c : str) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			result += std::tolower(static_cast<unsigned char>(c));
		}
	}
	return result;
}

// Function to check if the buffer matches any Name partially
bool partialMatchFilter(const char* buf_filter, std::string Name) {
	
	// Return false if the buffer is null or empty
	if (buf_filter == nullptr || buf_filter[0] == '\0') {
		return false;
	}


	// Normalize the buffer string
	std::string filterStr = normaliseString(buf_filter);

	std::string normalisedName = normaliseString(Name);
	if (normalisedName.find(filterStr) != std::string::npos) {
		return true; // Partial match found
	}

	return false; // No partial match found
}

// Function to split a string by a delimiter
std::vector<std::string> splitString(const std::string& str, char delimiter) {
	std::vector<std::string> tokens;
	std::stringstream ss(str);
	std::string token;
	while (std::getline(ss, token, delimiter)) {
		tokens.push_back(token);
	}
	return tokens;
}

bool isInputEmpty(const char* buffer) {
	if (buffer == nullptr) return true; // Null buffer is considered empty

	for (const char* p = buffer; *p != '\0'; ++p) {
		if (!std::isspace(static_cast<unsigned char>(*p))) {
			return false; // Found a non-whitespace character
		}
	}
	return true; // Buffer is empty or only contains whitespace
}

GlueMidi::GlueMidi(void (*func)())
	:callbackFunc(func)
{
	bDone = false;

	width = 600;
	height = 600;

	MidiOutIndex = 0;

	lastChannel = 0;
	lastCCnum = 0;
	lastCCvalue = 0;

	// Preallocate to avoid heap corruption at high frequencies
	MidiLogs.reserve(LINESMAX);

	// Try to read ini file
	iniFileName = "GlueMidisettings.ini";
	settings_were_loaded = readAppSettings(iniFileName, settings_pairs);

	StartMidiThread();
}

GlueMidi::~GlueMidi()
{
	releaseMidiPorts();
	StopMidiThread();

	for (auto& Item : InputItems)
	{
		if (Item)
		{
			closeMidiInput_Internal(*Item);
		}
	}

	InputItems.clear();

	if (midiout)
	{
		try
		{
			if (midiout->isPortOpen())
			{
				midiout->closePort();
			}
		}
		catch (...)
		{
		}

		delete midiout;
		midiout = nullptr;
	}

	delete midiin;
	midiin = nullptr;
}

void GlueMidi::EnqueueMidiMessage(uint64_t InputId, double DeltaTime, bool ShouldRoute, const std::vector<unsigned char>& Message)
{
	QueuedMidiMessage Queued;
	Queued.InputId = InputId;
	Queued.DeltaTime = DeltaTime;
	Queued.ShouldRoute = ShouldRoute;
	Queued.Data = Message;
	
	{
		std::lock_guard<std::mutex> Lock(IncomingMidiMutex);
		
		// don't let a broken MIDI device gobble unlimited memory
		if (IncomingMidiQueue.size() >= MaxIncomingMidiMessages)
		{
			IncomingMidiQueue.pop_front();
		}

		IncomingMidiQueue.push_back(std::move(Queued));
	}

	// unblock anything that's waiting on this
	IncomingMidiCondition.notify_one();
}

void GlueMidi::ProcessMidiMessageForMonitor(InputItem& Input, double deltatime, const std::vector<unsigned char>& message)
{
	AnimDeltaCounter += deltatime;


	if (message.size() < 1)
	{
		return;
	}

	int channel = 0;
	int note = 0;
	bool noteOff = false;
	int velocity = 0;
	int pressure = 0;
	int program = 0;
	int pitchbend = 0x2000;
	int ccNum = 0;
	int ccChan = 0;
	int ccValue = 0;
	bool is14bit = false;
	int value14bit = 0;

	std::stringstream finalhexout;

	// https://www.hinton-instruments.co.uk/reference/midi/protocol/index.htm

	const unsigned char statusByte = message[0];

	if (statusByte < 0xF0)
	{
		const unsigned char messageType = statusByte & 0xF0;

		const size_t requiredSize =
			(messageType == 0xC0 || messageType == 0xD0) ? 2 : 3;

		if (message.size() < requiredSize)
		{
			return;
		}
	}

	switch (statusByte >> 4)
	{
	case 0x08:	// Note Off
		channel = statusByte & 0x0F;
		note = (int)message.at(1);
		velocity = (int)message.at(2);
		break;

	case 0x09:	// Note On
		channel = statusByte & 0x0F;
		note = (int)message.at(1);
		velocity = (int)message.at(2);
		break;

	case 0x0a:	// Poly aftertouch
		channel = statusByte & 0x0F;
		note = (int)message.at(1);
		pressure = (int)message.at(2);
		break;

	case 0x0b:	// Control Change (or mode change)
		channel = statusByte & 0x0F;
		ccNum = (int)message.at(1);
		ccValue = (int)message.at(2);
		break;

	case 0x0c:	// Program Change
		channel = statusByte & 0x0F;
		program = (int)message.at(1);
		break;

	case 0x0d:	// Channel aftertouch
		channel = statusByte & 0x0F;
		pressure = (int)message.at(1);
		break;
	case 0x0e:	// Pitchbend
		channel = statusByte & 0x0F;
		int lsb = (int)message.at(1);
		int msb = (int)message.at(2);
		pressure = (msb << 7) | lsb;
		break;
	}

	// System Common
	switch (statusByte)
	{
	case 0xf0:	// Sysex start

		break;
	case 0xf1:	// Quarter Frame

		break;
	case 0xf2:	// Song Position Pointer

		break;
	case 0xf3:	// Song Select

		break;
	case 0xf4:	// undefined

		break;
	case 0xf5:	// undefined

		break;
	case 0xf6:	// Tune Request

		break;
	case 0xf7:	// Sysex end

		break;
	}

	// System Realtime
	switch (statusByte)
	{
	case 0xf8:	// Timing clock

		break;
	case 0xf9:	// undefined

		break;
	case 0xfa:	// Start

		break;
	case 0xfb:	// Continue

		break;
	case 0xfc:	// Stop

		break;
	case 0xfd:	// undefined

		break;
	case 0xfe:	// Active Sensing

		break;
	case 0xff:	// System Reset

		break;
	}



	// Is this a sysex message?
 	if (((int)message.at(0) == 0xF0) && (message.size() >= 14) && !displayRaw)
 	{
 		std::vector<unsigned char>::const_iterator it = message.begin();
 		// UNUSED - we're not formatting sysex yet...

		// until we DO deal with sysex, treat it as raw data
		for (const unsigned char Byte : message)
		{
			finalhexout
				<< std::setfill('0')
				<< std::setw(2)
				<< std::hex
				<< static_cast<int>(Byte)
				<< ' ';
		}
 	}




	// Is this a control message status byte? Check if MSB is 0B
	else if ((((int)message.at(0) >> 4) == 0x0b) && !displayRaw)
	{
		ccNum = (int)message.at(1);
		ccChan = ((int)message.at(0) & 0x0F) + 1;
		ccValue = (int)message.at(2);

		is14bit = false;
		value14bit = 0;
		// If this CC number is equal to the last one + 32, and the channel is the same, it's 14-bit o'clock
		if ((ccNum == (lastCCnum + 32)) && (ccChan == lastChannel))
		{
			is14bit = true;
			value14bit = (lastCCvalue << 7) | ccValue;
		}

		// It is, so get the channel from the LSB. We add 1 for display purposes.
		finalhexout << "Chan:" << ccChan << " ";

		// Control message
		finalhexout << "CC";

		// Next byte will be CC number
		finalhexout << ccNum;

		// Then value
		if (is14bit)
		{
			finalhexout << " Value 14bit: " << value14bit;
		}
		// Only display 7bit values if the 14bit filter is disabled
		else
		{
			finalhexout << " Value 7bit: " << ccValue;
		}

		// Cache channel and CC num for later 14-bit checks
		lastChannel = ccChan;
		lastCCnum = ccNum;
		lastCCvalue = ccValue;
	}

	// It's not CC or sysex so just display raw bytes
	else for (auto it = message.begin(); it != message.end(); it++)
	{
		finalhexout << std::setfill('0') << std::setw(sizeof(char) * 2) << std::hex << int(*it) << " ";
	}

	const bool isCC = (statusByte & 0xF0) == 0xB0;

	// Always display raw bytes if filter enabled
	if (!displayRaw)
	{
		// Die if channel filter is enabled and this doesn't match
		if (filterChannel >= 1 &&
			(!isCC || ccChan != filterChannel))
		{
			return;
		}

		// Die if CC filter is enabled and this doesn't match
		if (filterCC >= 0 &&
			(!isCC || ccNum != filterCC))
		{
			return;
		}

		// Die if this is a 7bit value but the 14-bit filter is enabled
		if (filter14bit && !is14bit)
		{
			return;
		}
	}


	// Is logging enabled for this input?
	if (!Input.LogMute)
	{
		if (!isInputEmpty(buf_filter))
		{
			const std::string filterStr = normaliseString(buf_filter);
			const std::vector<std::string> filters =
				splitString(filterStr, ',');

			for (const std::string& filter : filters)
			{
				if (filter.size() >= 3 &&
					partialMatchFilter(filter.c_str(), Input.Name))
				{
					Log((finalhexout.str() + "\t" + Input.Name).c_str());
					break;
				}
			}
		}
		else
		{
			Log((finalhexout.str() + "\t" + Input.Name).c_str());
		}
	}

	if (AnimDeltaCounter > AnimDeltaThreshold)
	{
		CallAnimate();
		AnimDeltaCounter = 0;
 	}
}

InputItem* GlueMidi::FindInputById(uint64_t InputId)
{
	for (auto& ItemPtr : InputItems)
	{
		if (ItemPtr && ItemPtr->Id == InputId)
		{
			return ItemPtr.get();
		}
	}

	return nullptr;
}

void GlueMidi::StartMidiThread()
{
	if (MidiThreadRunning_atomic.exchange(true))
	{
		return;
	}

	MidiThread = std::thread(&GlueMidi::MidiThreadMain, this);
}

void GlueMidi::StopMidiThread()
{
	if (!MidiThreadRunning_atomic.exchange(false))
	{
		return;
	}

	IncomingMidiCondition.notify_all();

	if (MidiThread.joinable())
	{
		MidiThread.join();
	}
}

void GlueMidi::MidiThreadMain()
{
	// Main worker thread loop
	while (MidiThreadRunning_atomic.load(std::memory_order_acquire))
	{
		{
			std::unique_lock<std::mutex> Lock(IncomingMidiMutex);

			IncomingMidiCondition.wait(
				Lock,
				[this]
				{
					return
						!MidiThreadRunning_atomic.load(std::memory_order_acquire)
						|| !IncomingMidiQueue.empty()
						|| HasPendingMidiCommands();
				});
		}

		if (!MidiThreadRunning_atomic.load(std::memory_order_acquire))
			break;

		ProcessPendingMidiCommands();

		// Then drain every queued MIDI message
		while (true)
		{
			QueuedMidiMessage Message;

			{
				std::lock_guard<std::mutex> Lock(IncomingMidiMutex);

				if (IncomingMidiQueue.empty())
					break;

				Message = std::move(IncomingMidiQueue.front());
				IncomingMidiQueue.pop_front();
			}

			if (Message.ShouldRoute)
			{
				SendMessageOnPort(&Message.Data, midiout);
			}

			{
				std::lock_guard<std::mutex> Lock(MonitorMidiMutex);
				MonitorMidiQueue.push_back(std::move(Message));
			}
		}
	}

	
}

void GlueMidi::ProcessMonitorMidiMessages()
{
	//drain the worker thread monitor queue
	std::deque<QueuedMidiMessage> LocalQueue;

	{
		std::lock_guard<std::mutex> Lock(MonitorMidiMutex);
		LocalQueue.swap(MonitorMidiQueue);
	}

	for (const QueuedMidiMessage& Message : LocalQueue)
	{
		InputItem* Input = FindInputById(Message.InputId);

		if (Input == nullptr)
		{
			continue;
		}

		ProcessMidiMessageForMonitor(
			*Input,
			Message.DeltaTime,
			Message.Data);
	}
}

void GlueMidi::QueueOpenMidiOutput(const int OutputIndex)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::OpenOutput;
	Command.OutputIndex = OutputIndex;

	{
		std::lock_guard<std::mutex> Lock(MidiCommandMutex);
		MidiCommandQueue.push_back(std::move(Command));
	}

	IncomingMidiCondition.notify_one();
}

void GlueMidi::QueueCloseMidiOutput()
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::CloseOutput;

	{
		std::lock_guard<std::mutex> Lock(MidiCommandMutex);
		MidiCommandQueue.push_back(std::move(Command));
	}

	IncomingMidiCondition.notify_one();
}

void GlueMidi::QueueOpenMidiInput(const int InputIndex)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::OpenInput;
	Command.InputIndex = InputIndex;

	{
		std::lock_guard<std::mutex> Lock(MidiCommandMutex);
		MidiCommandQueue.push_back(std::move(Command));
	}

	IncomingMidiCondition.notify_one();
}

void GlueMidi::QueueCloseMidiInput(const int InputIndex)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::CloseInput;
	Command.InputIndex = InputIndex;

	{
		std::lock_guard<std::mutex> Lock(MidiCommandMutex);
		MidiCommandQueue.push_back(std::move(Command));
	}

	IncomingMidiCondition.notify_one();
}

void GlueMidi::QueueRefreshPorts()
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::RefreshPorts;

	{
		std::lock_guard<std::mutex> Lock(MidiCommandMutex);
		MidiCommandQueue.push_back(std::move(Command));
	}

	IncomingMidiCondition.notify_one();
}

void GlueMidi::QueueReleaseMidi()
{

}

bool GlueMidi::HasPendingMidiCommands()
{
	std::lock_guard<std::mutex> Lock(MidiCommandMutex);

	return !MidiCommandQueue.empty();
}

void GlueMidi::ProcessPendingMidiCommands()
{
	while (true)
	{
		MidiCommand Command;

		{
			std::lock_guard<std::mutex> Lock(MidiCommandMutex);

			if (MidiCommandQueue.empty())
				break;

			Command = std::move(MidiCommandQueue.front());
			MidiCommandQueue.pop_front();
		}

		switch (Command.Type)
		{
		case EMidiCommandType::CloseOutput:

			try
			{
				if (midiout && midiout->isPortOpen())
				{
					midiout->closePort();
					CurrentOutputIndex_atomic.store(-1, std::memory_order_release);
					MidiOutputOpen_atomic.store(false);
				}					
			}
			catch (RtMidiError& Error)
			{
				MidiOutputOpen_atomic.store(false, std::memory_order_release);
				CurrentOutputIndex_atomic.store(-1, std::memory_order_release);
				Error.printMessage();
			}

			break;

		case EMidiCommandType::OpenOutput:
		{
			const int Index = Command.OutputIndex;

			if (!midiout)
				return;

			if (Index < 0 || Index >= static_cast<int>(MidiOutNames.size()))
				return;

			try
			{
				if (midiout->isPortOpen())
					midiout->closePort();

				midiout->openPort(Index, MidiOutNames[Index]);
				CurrentOutputIndex_atomic.store(Index, std::memory_order_release);
				MidiOutputOpen_atomic.store(true);
					
			}
			catch (RtMidiError& Error)
			{
				MidiOutputOpen_atomic.store(false, std::memory_order_release);
				CurrentOutputIndex_atomic.store(-1, std::memory_order_release);
				Error.printMessage();
			}

			break;
		}
		case EMidiCommandType::OpenInput:
		{
			openMidiInput_Internal(Command.InputIndex);
			break;
		}


		case EMidiCommandType::CloseInput:
		{
			for (auto& ItemPtr : InputItems)
			{
				if (ItemPtr && ItemPtr->Index == Command.InputIndex)
				{
					closeMidiInput_Internal(*ItemPtr);
					break;
				}
			}

			break;
		}

		case EMidiCommandType::RefreshPorts:
		{
			refreshPorts_Internal();
			
			break;
		}
		case EMidiCommandType::ReleaseAll:
		{
			for (auto& Item : InputItems)
			{
				if (Item)
				{
					closeMidiInput_Internal(*Item);
				}
			}

			if (midiout && midiout->isPortOpen())
			{
				midiout->closePort();

				MidiOutputOpen_atomic.store(false, std::memory_order_release);
				CurrentOutputIndex_atomic.store(-1, std::memory_order_release);
			}
			break;
		}

		}
	}	
}

void GlueMidi::Update()
{
	ProcessMonitorMidiMessages();

	// Start ImGui frame
	ImGui_ImplWin32_NewFrame();
	ImGui_ImplDX11_NewFrame();
	ImGui::NewFrame();


	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x - 1, viewport->Pos.y));
	//ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

	
	if (ImGui::Begin("##GlueMidi", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar))
	{
		// Keep these updated for saving on close		
		width = (int)ImGui::GetWindowWidth();
		height = (int)ImGui::GetWindowHeight();


		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{				

				ImGui::Separator();

				if (ImGui::MenuItem("Quit"))
				{
					bDone = true;
				}
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Options"))
			{
				static bool closetotraytoggle = (bool)GetConfigInt("closetotray", true);
				if (ImGui::MenuItem("Minimise to tray on close", NULL, &closetotraytoggle))
				{
					SetConfigInt("closetotray", closetotraytoggle);
					SaveSettings();
				}

				static bool starttraytoggle = (bool)GetConfigInt("startintray", true);
				if (ImGui::MenuItem("Start minimised to tray", NULL, &starttraytoggle))
				{
					SetConfigInt("startintray", starttraytoggle);
					SaveSettings();
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Theme"))
			{
				if (ImGui::MenuItem("Fader3"))
				{
					Fader3ImGuiStyle();
				}

				if (ImGui::MenuItem("VisualStudio"))
				{
					SetupImGuiStyle();
				}

				if (ImGui::MenuItem("Bess Dark"))
				{
					SetBessTheme();
				}

				if (ImGui::MenuItem("ImGui Dark"))
				{
					ImGui::StyleColorsDark();
				}

				ImGui::EndMenu();
			}

			ImGui::EndMenuBar();
		}


		if (ImGui::Button(ICON_FA_RECYCLE" Refresh"))
		{
			refreshMidiPorts();
		}
		ImGui::SetItemTooltip("Empty the list of discovered\n MIDI ports and search again");

		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_PLUG_CIRCLE_XMARK " Release All"))
		{
			releaseMidiPorts();
		}
		ImGui::SetItemTooltip("Close any open MIDI ports so\n other programs can use them");


		ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV;
		if (ImGui::BeginTable("##mainTable", 2, tableFlags))
		{
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			{
				
				ImGui::Text("Inputs");

				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginListBox("##InputsList", ImVec2(-1, (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y) * InputItems.size() * 2.0f))) {				

					for (int i = 0; i < InputItems.size(); i++)
					{
						auto& Item = *InputItems[i];

						ImGui::PushID(i);						

						const bool IsMuted = Item.Muted_atomic.load(std::memory_order_relaxed);

						if (!IsMuted)
						{
							if (ImGui::Button(ICON_FA_PAUSE))
							{
								Item.Muted_atomic.store(!IsMuted, std::memory_order_relaxed);
								
							}
							ImGui::SetItemTooltip("Pause output");
						}
						else
						{
							if (ImGui::Button(ICON_FA_PLAY))
							{
								Item.Muted_atomic.store(!IsMuted, std::memory_order_relaxed);
							}
							ImGui::SetItemTooltip("Resume output");
						}
						
						

						ImGui::SameLine();

						bool selected = Item.Active_atomic.load();

						if (ImGui::Selectable(Item.Name.c_str(), &selected))
						{
							if (selected)
							{
								QueueOpenMidiInput(Item.Index);
								Item.Active_atomic.store(true);
								UpdateInputsConfig(Item.Name);
								Log((Item.Name + " OPENED").c_str());
								SaveSettings();
							}
							else
							{
								QueueCloseMidiInput(Item.Index);
								
								UpdateInputsConfig(Item.Name, true); // remove
								Log((Item.Name + " CLOSED").c_str());
								SaveSettings();
							}
						}
						ImGui::PopID();
					}
					ImGui::EndListBox();
				}

			}


			ImGui::TableSetColumnIndex(1);
			{
				static ImVec4 outputStatusCol = ImVec4(1.0, 0.0, 0.0, 1.0); // red

				ImGui::Text("Output");
				ImGui::SameLine();
				
				ImGui::TextColored(outputStatusCol, outputStatus);
				
				// Reset this in case we've released the ports 
				const bool OutPortIsOpen =
					MidiOutputOpen_atomic.load(std::memory_order_acquire);

				snprintf(
					outputStatus,
					sizeof(outputStatus),
					OutPortIsOpen ? "Active" : "Inactive");

				// MIDI out listbox
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginListBox("##outlist", ImVec2(-1, (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y) * MidiOutNames.size() * 2.0)))
				{
					// MIDI out selectables
					for (int k = 0; k < MidiOutNames.size(); k++)
					{
						ImGui::PushID(k);
						bool selected = MidiOutIndex == k;

						if (ImGui::Selectable((MidiOutNames[k] + "##2").c_str(), &selected))
						{
							MidiOutIndex = k;

							QueueOpenMidiOutput(k);

							SetConfigString(
								"outmidi",
								MidiOutNames[MidiOutIndex]);

							SaveSettings();
						}
						ImGui::PopID();
					}

					ImGui::EndListBox();
				}

				if (strcmp(outputStatus, "Active") == 0)
				{
					outputStatusCol = ImVec4(0.0, 1.0, 0.0, 1.0);
				}
				else
				{
					outputStatusCol = ImVec4(1.0, 0.0, 0.0, 1.0);
				}				
			}

			ImGui::EndTable();
		}


		
		ImGui::Separator();


		ImGui::Text("Midi Monitor");

		ImGui::AlignTextToFramePadding();
		ImGui::Text("Filters: ");
		ImGui::SameLine();
		static bool c_shownotes = filterShowNotes;
		if (ImGui::Checkbox("Notes", &c_shownotes))
		{
			filterShowNotes = c_shownotes;
		}
		ImGui::SameLine();
		static bool c_showcc = filterShowCC;
		if (ImGui::Checkbox("CC", &c_showcc))
		{
			filterShowCC = c_showcc;
		}
		ImGui::SameLine();
		static bool c_showsys = filterShowSys;
		if (ImGui::Checkbox("Sys", &c_showsys))
		{
			filterShowSys = c_showsys;
		}
		ImGui::SameLine();
		static bool c_showsysex = filterShowSysex;
		if (ImGui::Checkbox("SysEx", &c_showsysex))
		{
			filterShowSysex = c_showsysex;
		}
		ImGui::SameLine();
		static bool c_showraw = filterShowRaw;
		if (ImGui::Checkbox("Raw", &c_showraw))
		{
			filterShowRaw = c_showraw;
		}

		ImGui::SameLine();
		ImGuiInputTextFlags InTextFlags = ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_AutoSelectAll;

		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0);
		ImGui::InputText("##FilterInput", buf_filter, 32, InTextFlags);
		ImGui::SetItemTooltip("Filter Input names with 3 or more matching characters.\nUse ',' to separate multiple filters.\nEmpty filter logs all inputs.");
		
		if (!isInputEmpty(buf_filter))
		{
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_XMARK))
			{
				buf_filter[0] = '\0';
			}
		}		
		

		
		static ImGuiInputTextFlags flags = ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll;

		std::string multilines;
		for (int i = (int)MidiLogs.size() - 1; i > 0; i--)
		{
			if (i >= LINESMAX) continue;

			multilines += MidiLogs[i] + '\n';
		};

		static std::vector<char> multibuffer;
		multibuffer.resize(multilines.size() + 1); // leave space for null terminator
		std::copy(multilines.begin(), multilines.end(), multibuffer.begin());
		multibuffer[multilines.size()] = '\0'; // terminate buffer

		ImGui::InputTextMultiline("##logoutput", multibuffer.data(), multibuffer.size(), ImVec2(-FLT_MIN, -1.0), flags);


		// Main ImGui window ENDS
		ImGui::End();
	}
	// ImGui demo window
	//ImGui::ShowDemoWindow();

	// Render
	ImGui::Render();
}

std::string GlueMidi::getExecutableDirectory()
{
	char path[1024] = { 0 };

#ifdef _WIN32 
	GetModuleFileNameA(nullptr, path, sizeof(path));
	std::string exePath(path);
	size_t pos = exePath.find_last_of("\\/");
	return exePath.substr(0, pos); // Get directory of executable
#else
	// Mac or Linux
	char result[1024];
	SSIZE_T count = readlink("/proc/self/exe", result, sizeof(result));
	if (count != -1)
	{
		std::string exePath(result, count);
		size_t pos = exePath.find_last_of("/\\");
		return exePath.substr(0, pos); // return dir of exe
	}
	return "";
#endif
}

void GlueMidi::writeAppSettings(const std::string& fileName, const std::unordered_map<std::string, std::string>& keyValuePairs)
{
	std::string filePath = getExecutableDirectory() + "/" + fileName;
	std::ofstream file(filePath);
	if (file.is_open())
	{
		file << "[Settings]" << std::endl;

		for (const auto& pair : keyValuePairs)
		{
			file << pair.first << "=" << pair.second << std::endl;
		}

		file.close();
		std::cout << "Data written to " << filePath << std::endl;
	}
	else
	{
		std::cerr << "Failed to open file for writing!" << std::endl;
	}
}

bool GlueMidi::readAppSettings(const std::string& fileName, std::unordered_map<std::string, std::string>& keyValuePairs)
{	
	std::string filePath = getExecutableDirectory() + "/" + fileName;
	std::ifstream file(filePath);

	if (file.is_open()) {
		std::string line;
		std::string currentSection;

		while (std::getline(file, line))
		{
			if (line.empty() || line[0] == ';' || line[0] == '#')
				continue;

			if (line[0] == '[' && line[line.size() - 1] == ']')
			{
				currentSection = line.substr(1, line.size() - 2);
				continue;
			}

			// Process key/value pairs
			size_t delimiterPos = line.find('=');
			if (delimiterPos != std::string::npos)
			{
				std::string key = line.substr(0, delimiterPos);
				std::string value = line.substr(delimiterPos + 1);

				// Store in the map
				keyValuePairs[key] = value;
			}
		}

		std::cout << "Data read from " << filePath << std::endl;
		file.close();
		return true;
	}
	else {
		std::cerr << "Failed to open file for reading!" << std::endl;
		return false;
	}
	file.close();

	return false;
}


bool GlueMidi::findConfigValue(const std::string keyname, std::unordered_map<std::string, std::string>& configPairsLoading, int* outputint /*= nullptr*/, std::string* outputstring /*= nullptr*/)
{
	if (configPairsLoading.find(keyname) != configPairsLoading.end())
	{
		try
		{
			if (outputint != nullptr)
			{
				// Only overwrite output if we find a valid keyname
				*outputint = std::stoi(configPairsLoading[keyname]);
			}
			if (outputstring != nullptr)
			{
				// Only overwrite output if we find a valid keyname
				*outputstring = configPairsLoading[keyname];
			}

			return true;
		}
		catch (const std::invalid_argument& e)
		{
			std::cerr << "Invalid argument: " << e.what() << std::endl;
			return false;
		}
		catch (const std::out_of_range& e) {
			std::cerr << "Out of range: " << e.what() << std::endl;
			return false;
		}

	}
	return false;
}



void GlueMidi::setupImGuiFonts()
{
	ImGuiIO& io = ImGui::GetIO();

	io.Fonts->AddFontDefault();

	static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
	ImFontConfig config;
	config.MergeMode = true;
	config.PixelSnapH = true;
	config.GlyphMaxAdvanceX = iconFontSize;
	config.GlyphOffset.y = 1.0f;
	iconFont = io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, iconFontSize, &config, icon_ranges);

	io.Fonts->Build();
}

int GlueMidi::refreshMidiPorts()
{
	QueueRefreshPorts();
	return 1;
}

void GlueMidi::refreshPorts_Internal()
{
	for (auto& Item : InputItems)
	{
		if (Item)
		{
			closeMidiInput_Internal(*Item);
		}
	}

	clearIncomingMidiQueue();
	InputItems.clear();

	MidiOutputOpen_atomic.store(false, std::memory_order_release);
	CurrentOutputIndex_atomic.store(-1, std::memory_order_release);



	try
	{
		if (midiout->isPortOpen())
		{
			midiout->closePort();
		}
	}
	catch (RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage().c_str());
	}

	InPortCount = 0;
	MidiOutNames.clear();

	try
	{
		InPortCount = midiin->getPortCount();

		for (unsigned int Index = 0; Index < InPortCount; ++Index)
		{
			const std::string PortName =
				midiin->getPortName(Index);

			auto Item =
				std::make_unique<InputItem>(
					this,
					PortName,
					Index);

			Item->Id = NextInputId++;

			Log(Item->Name.c_str());

			InputItems.push_back(std::move(Item));
		}
	}
	catch (RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage().c_str());
		return;
	}

	try
	{
		const unsigned int OutputPortCount =
			midiout->getPortCount();

		for (unsigned int Index = 0;
			Index < OutputPortCount;
			++Index)
		{
			const std::string PortName =
				midiout->getPortName(Index);

			const std::string TrimmedName =
				PortName.substr(
					0,
					PortName.find_last_of(' '));

			MidiOutNames.push_back(TrimmedName);
			Log(TrimmedName.c_str());
		}
	}
	catch (RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage().c_str());
		return;
	}

	Log("MIDI ports refreshed successfully");

	if (settings_were_loaded)
	{
		reopenSavedPorts();
		Log("Saved port reopen requested");
	}
}

void GlueMidi::reopenSavedPorts()
{	
// Attempt to open the previously used MIDI device ports.
// Note that we copy the config array first, and don't remove any ports that
// we don't find in the current MidiInNames. Maybe you'll plug it in next time,
	const std::vector<std::string> PrevMidiIns = GetConfigStringArray("inmidis");

	for (const auto& PrevMidi : PrevMidiIns)
	{
		for (auto& Item : InputItems)
		{
			if (Item->Name == PrevMidi)
			{
				QueueOpenMidiInput(Item->Index);
				break;
			}
		}
	}


	const std::string PrevMidiOut = GetConfigString("outmidi");

	for (int i = 0; i < static_cast<int>(MidiOutNames.size()); ++i)
	{
		if (MidiOutNames[i] == PrevMidiOut)
		{
			MidiOutIndex = i;
			QueueOpenMidiOutput(i);
			break;
		}
	}
}

bool GlueMidi::portCountHasChanged()
{
	if (!midiin)
	{
		try
		{
			midiin = new RtMidiIn();
			midiin->setBufferSize(2048, 4);
			midiout = new RtMidiOut();
		}
		catch (RtMidiError& error)
		{
			std::cerr << "RtMidiError: ";
			error.printMessage();
			Log(error.getMessage().c_str());
			//exit(EXIT_FAILURE);
			return false;
		}
	}

	if (midiin)
	{
		// All we want to do is see if the number of ports has changed

		unsigned int nPorts = midiin->getPortCount();

		if (midiin->getPortCount() != InPortCount)
		{
			return true;
		}		
	}

	return false;
}

// We need to use a static var for the main GlueMidi instance because it's for some reason 
// impossible to pass RtMidi our callback function when it's a member of a class, and I 
// couldn't get it to work as a lambda either.
static GlueMidi* globalInstance = nullptr;

static void midiInCallback(double deltatime, std::vector<unsigned char>* message, void* userData )
{
	if(message == nullptr || message->empty() || userData == nullptr)
		return;

	auto* Input = static_cast<InputItem*>(userData);

	if (!Input->AcceptCallbacks_atomic.load(std::memory_order_acquire))
	{
		return;
	}

	if(Input->gluemidi == nullptr)
		return;

	const bool shouldRoute = !Input->Muted_atomic.load(std::memory_order_relaxed);

	Input->gluemidi->EnqueueMidiMessage(Input->Id, deltatime, shouldRoute, *message);

}

void GlueMidi::openMidiInput_Internal(const int InputIndex)
{
	if (InputIndex < 0 ||
		InputIndex >= static_cast<int>(InputItems.size()))
	{
		return;
	}

	InputItem& Item = *InputItems[InputIndex];

	if (!Item.midiinput)
	{
		Item.midiinput = std::make_unique<RtMidiIn>();

		Item.midiinput->setBufferSize(2048, 4);
	}

	Item.AcceptCallbacks_atomic.store(false);

	try
	{
		Item.midiinput->setCallback(
			midiInCallback,
			&Item);

		Item.midiinput->ignoreTypes(false, true, true);

		Item.midiinput->openPort(
			Item.Index,
			Item.NameIndexed);

		Item.AcceptCallbacks_atomic.store(true);

		Item.Active_atomic.store(true);

		Log((Item.Name + " OPEN").c_str());
	}
	catch (RtMidiError& Error)
	{
		Item.AcceptCallbacks_atomic.store(false);

		Error.printMessage();

		Log(Error.getMessage().c_str());
	}
}




void GlueMidi::closeOutput_Internal()
{
	try
	{
		if (midiout && midiout->isPortOpen())
		{
			midiout->closePort();
			CurrentOutputIndex_atomic.store(-1, std::memory_order_release);
			MidiOutputOpen_atomic.store(false);
		}
	}
	catch (RtMidiError& Error)
	{
		MidiOutputOpen_atomic.store(false, std::memory_order_release);
		CurrentOutputIndex_atomic.store(-1, std::memory_order_release);
		Error.printMessage();
	}
}

void GlueMidi::closeMidiInput_Internal(InputItem& Input)
{
	Input.AcceptCallbacks_atomic.store(false, std::memory_order_release);

	if (Input.midiinput)
	{
		try
		{
			Input.midiinput->cancelCallback();

			if (Input.midiinput->isPortOpen())
			{
				Input.midiinput->closePort();
			}
		}
		catch (RtMidiError& Error)
		{
			Error.printMessage();
			Log(Error.getMessage().c_str());
		}

		Input.midiinput.reset();
	}

	Input.Active_atomic.store(false);
}

void GlueMidi::releaseMidiPorts()
{
	{
		std::lock_guard<std::mutex> Lock(MidiCommandMutex);

		MidiCommandQueue.emplace_back(
			EMidiCommandType::ReleaseAll);
	}

	IncomingMidiCondition.notify_one();
}

void GlueMidi::SendMessageOnPort(const std::vector<unsigned char>* OutMsg, RtMidiOut* OutInstance)
{
	if (OutInstance)
	{
		if (OutInstance->isPortOpen())
		{
			try
			{
				OutInstance->sendMessage(OutMsg);
			}
			catch (RtMidiError& error)
			{
				std::cerr << "RtMidiError: ";
				error.printMessage();
				return;
			}

		}
	}
}

void GlueMidi::Log(const char* fmt) {
	
	std::time_t now = std::time(nullptr);
	std::tm localTime;
	localtime_s(&localTime, &now);

	std::ostringstream timestamp;
	timestamp << "[" << std::setfill('0') << std::setw(2) << localTime.tm_hour << ":"
		<< std::setfill('0') << std::setw(2) << localTime.tm_min << ":"
		<< std::setfill('0') << std::setw(2) << localTime.tm_sec << "] ";

	std::string timestampedFmt = timestamp.str() + fmt;

	char buffer[LINEBUFFERMAX];
	snprintf(buffer, sizeof(buffer), "%s", timestampedFmt.c_str());
	buffer[sizeof(buffer) - 1] = 0; // Ensure null-termination
	MidiLogs.push_back(buffer);



	// Lose surplus lines
	std::lock_guard<std::mutex> lock(midiLogMutex);
	if (MidiLogs.size() > LINESMAX)
	{
		size_t excess = MidiLogs.size() - LINESMAX;
		MidiLogs.erase(MidiLogs.begin(), MidiLogs.begin() + excess);
	}
	
}

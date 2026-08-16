#include "GlueMidi.h"
#include "GlueMidiWorker.h"
#include "Version.h"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include "IconsFontAwesome6.h"
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <windows.h>

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

	lastChannel = 0;
	lastCCnum = 0;
	lastCCvalue = 0;

	// don't hammer reallocations while appending log lines
	MidiLogs.reserve(LINESMAX);

	// Try to read ini file
	iniFileName = "GlueMidisettings.ini";
	settings_were_loaded = readAppSettings(iniFileName, settings_pairs);

	// create the worker and start it
	MidiWorker_ = std::make_unique<GlueMidiWorker>(
		[this](const std::string& Message)
		{
			Log(Message.c_str());
		});

	MidiWorker_->Start();

	MidiWorker_->QueueRefreshPorts();
}

GlueMidi::~GlueMidi()
{
	// Teardown is cleaner now, since StopMidiThread schedules an orderly
	// shutdown (port closures, thread disabling, etc)
	
	if (MidiWorker_)
	{
		MidiWorker_->Stop();
		MidiWorker_.reset();
	}

	// midiin/midiout now created and destroyed on the worker thread
}

void GlueMidi::ProcessMidiMessageForMonitor(const MidiInputUiState& Input, double deltatime, const std::vector<unsigned char>& message)
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
		// If this CC number is equal to the last one + 32, and the channel is the same, it's 14-bit o'clock!
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
		if (filterChannel >= 1 && (!isCC || ccChan != filterChannel))
		{
			return;
		}

		// Die if CC filter is enabled and this doesn't match
		if (filterCC >= 0 && (!isCC || ccNum != filterCC))
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
			const std::vector<std::string> filters = splitString(filterStr, ',');

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

void GlueMidi::ProcessMonitorMidiMessages()
{
	if (!MidiWorker_)
	{
		return;
	}

	//drain the worker thread monitor queue
	std::vector<QueuedMidiMessage> Messages = MidiWorker_->DrainMonitorMessages();

	const MidiPortUiSnapshot Snapshot = MidiWorker_->GetSnapshot();

	for (const QueuedMidiMessage& Message : Messages)
	{
		const MidiInputUiState* Input = nullptr;

		for (const MidiInputUiState& Candidate :
			Snapshot.Inputs)
		{
			if (Candidate.Id == Message.InputId)
			{
				Input = &Candidate;
				break;
			}
		}

		if (Input == nullptr)
		{
			continue;
		}

		ProcessMidiMessageForMonitor(*Input, Message.DeltaTime, Message.Data);
	}
}

void GlueMidi::Update()
{
	ProcessMonitorMidiMessages();

	// ONLY use the inpyuts and outputs in Snapshot for UI stuff - no direct
	// access or mutation of RT-side ins/outs
	const MidiPortUiSnapshot Snapshot = MidiWorker_ ? MidiWorker_->GetSnapshot() : MidiPortUiSnapshot{};

	if (!bSavedPortsApplied && (!Snapshot.Inputs.empty() || !Snapshot.Outputs.empty()))
	{
		reopenSavedPorts();
		bSavedPortsApplied = true;
	}

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

		bool OpenAboutPopup = false;

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

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Help"))
			{
				if (ImGui::MenuItem("About..."))
				{					
					OpenAboutPopup = true;
				}

				ImGui::EndMenu();
			}
			
			ImGui::EndMenuBar();
		}

		if (OpenAboutPopup)
		{
			ImGui::OpenPopup("About");
		}

		ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		if (ImGui::BeginPopupModal("About", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("GlueMidi %s", GLUEMIDI_VERSION_STRING);

			ImGui::Separator();

			ImGui::PushTextWrapPos(ImGui::GetMainViewport()->Size.x - 20.f);

			ImGui::TextUnformatted("GlueMidi is a lightweight MIDI utility for Windows, designed "  
						"to allow simple 'many to one' routing of MIDI messages between "
						"virtual and hardware devices.\n\n"
						"It supports multiple simultaneous MIDI inputs, a single selectable "
						"MIDI output, live message monitoring with filtering, automatic device "
						"reconnection, hot-plug detection, and automatic saving/loading of your "
						"routing map. \n\n"
						"It's designed to be robust against unreliable drivers, though further "
						"stability improvements are in the pipeline.\n\n");

			ImGui::Separator();

			ImGui::TextUnformatted("GlueMidi is open source and released under the MIT license:\n"
						"https://github.com/echolevel/GlueMidi\n\n"
						"Copyright (c) 2026 Brendan O'Callaghan Ratliff"
						);

			ImGui::PopTextWrapPos();

			if (ImGui::Button("Close"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
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
				if (ImGui::BeginListBox("##InputsList", ImVec2(-1, (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y) * Snapshot.Inputs.size() * 2.0f))) {				

					for (int i = 0; i < Snapshot.Inputs.size(); i++)
					{
						const MidiInputUiState& Item = Snapshot.Inputs[i];

						ImGui::PushID(i);						

						
						if (!Item.Muted)
						{
							if (ImGui::Button(ICON_FA_PAUSE))
							{
								if(MidiWorker_)
									MidiWorker_->QueueSetInputMuted(Item.Index, true);
							}
							ImGui::SetItemTooltip("Pause output");
						}
						else
						{
							if (ImGui::Button(ICON_FA_PLAY))
							{
								if(MidiWorker_)
									MidiWorker_->QueueSetInputMuted(Item.Index, false);
							}
							ImGui::SetItemTooltip("Resume output");
						}
						
						

						ImGui::SameLine();

						bool selected = Item.Active;

						if (ImGui::Selectable(Item.Name.c_str(), &selected))
						{
							if (selected)
							{
								if(MidiWorker_)
								{
									MidiWorker_->QueueOpenInput(Item.Index);
									UpdateInputsConfig(Item.Name);
									Log(("Input: " + Item.Name + " OPENED").c_str());
								}
							}
							else
							{
								if (MidiWorker_)
								{
									MidiWorker_->QueueCloseInput(Item.Index);
									UpdateInputsConfig(Item.Name, true); // remove
									Log(("Input: " + Item.Name + " CLOSED").c_str());
								}
							}

							SaveSettings();
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
				
				snprintf(
					outputStatus,
					sizeof(outputStatus),
					Snapshot.OutputOpen ? "Active" : "Inactive");

				ImGui::TextColored(outputStatusCol, outputStatus);

				// MIDI out listbox
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginListBox("##outlist", ImVec2(-1, (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y) * Snapshot.Outputs.size() * 2.0f)))
				{
					// MIDI out selectables
					for (int k = 0; k < Snapshot.Outputs.size(); k++)
					{
						ImGui::PushID(k);
						const bool selected = Snapshot.OutputOpen && Snapshot.CurrentOutputIndex == k;

						if (ImGui::Selectable((Snapshot.Outputs[k]).c_str(), selected))
						{
							if (MidiWorker_)
							{
								MidiWorker_->QueueOpenOutput(k);

								Log(("Output: " + Snapshot.Outputs[k] + " OPENED").c_str());

								SetConfigString(
									"outmidi",
									Snapshot.Outputs[k]);

								SaveSettings();
							}
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

		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
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

		std::vector<std::string> LocalMidiLogs;

		{
			std::lock_guard<std::mutex> Lock(midiLogMutex);
			LocalMidiLogs = MidiLogs;
		}

		std::string multilines;
		for (int i = (int)LocalMidiLogs.size() - 1; i > 0; i--)
		{
			if (i >= LINESMAX) continue;

			multilines += LocalMidiLogs[i] + '\n';
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
	if (MidiWorker_)
	{
		MidiWorker_->QueueRefreshPorts();
		return 1;
	}

	return 0;
}

void GlueMidi::reopenSavedPorts()
{	
	if (!MidiWorker_)
	{
		return;
	}

// Attempt to open the previously used MIDI device ports.
// Note that we copy the config array first, and don't remove any ports that
// we don't find in the current MidiInNames. Maybe you'll plug it in next time,

	const MidiPortUiSnapshot Snapshot = MidiWorker_->GetSnapshot();

	const std::vector<std::string> SavedInputs = GetConfigStringArray("inmidis");

	for (const std::string& SavedName : SavedInputs)
	{
		for (const MidiInputUiState& Input : Snapshot.Inputs)
		{
			if (Input.Name == SavedName)
			{
				MidiWorker_->QueueOpenInput(Input.Index);
				break;
			}
		}
	}

	const std::string SavedOutput =
		GetConfigString("outmidi");

	for (int Index = 0; Index < static_cast<int>(Snapshot.Outputs.size()); ++Index)
	{
		if (Snapshot.Outputs[Index] == SavedOutput)
		{
			MidiWorker_->QueueOpenOutput(Index);
			break;
		}
	}
}

void GlueMidi::releaseMidiPorts()
{
	if (MidiWorker_)
	{
		MidiWorker_->QueueReleaseAll();
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

	const std::string Line = timestamp.str() + fmt;

	std::lock_guard<std::mutex> Lock(midiLogMutex);

	MidiLogs.push_back(Line);

	if (MidiLogs.size() > LINESMAX)
	{
		const size_t Excess = MidiLogs.size() - LINESMAX;

		MidiLogs.erase(
			MidiLogs.begin(),
			MidiLogs.begin() + Excess);
	}
	
}

std::string GlueMidi::GetConfigString(const std::string& Key, bool CreateIfNotFound /*= false*/)
{
	std::lock_guard<std::mutex> Lock(SettingsMutex);

	const auto It = settings_pairs.find(Key);

	if (It != settings_pairs.end())
	{
		return It->second;
	}

	if (CreateIfNotFound)
	{
		settings_pairs[Key] = "";
	}

	return "";
}

int GlueMidi::GetConfigInt(const std::string& Key, bool CreateIfNotFound /*= false*/)
{
	std::lock_guard<std::mutex> Lock(SettingsMutex);

	const auto It = settings_pairs.find(Key);

	if (It == settings_pairs.end())
	{
		if (CreateIfNotFound)
		{
			settings_pairs[Key] = "-1";
		}

		return -1;
	}

	try
	{
		return std::stoi(It->second);
	}
	catch (const std::exception&)
	{
		return -1;
	}
}

std::vector<std::string> GlueMidi::GetConfigStringArray(const std::string& Key, bool CreateIfNotFound /*= false*/)
{
	std::lock_guard<std::mutex> Lock(SettingsMutex);

	std::vector<std::string> Result;

	const auto It = settings_pairs.find(Key);

	if (It == settings_pairs.end())
	{
		if (CreateIfNotFound)
		{
			settings_pairs[Key] = "";
		}

		return Result;
	}

	if (It->second.empty())
	{
		return Result;
	}

	size_t Start = 0;

	while (Start <= It->second.size())
	{
		const size_t End = It->second.find(',', Start);

		if (End == std::string::npos)
		{
			Result.push_back(It->second.substr(Start));
			break;
		}

		Result.push_back(
			It->second.substr(Start, End - Start));

		Start = End + 1;
	}

	return Result;
}

void GlueMidi::SetConfigString(const std::string& Key, const std::string& Value)
{
	std::lock_guard<std::mutex> Lock(SettingsMutex);
	settings_pairs[Key] = Value;
}

void GlueMidi::SetConfigInt(const std::string& Key, const int Value)
{
	std::lock_guard<std::mutex> Lock(SettingsMutex);
	settings_pairs[Key] = std::to_string(Value);
}

void GlueMidi::SetConfigStringArray(const std::string& Key, const std::vector<std::string>& Values)
{
	std::ostringstream Stream;

	for (size_t Index = 0; Index < Values.size(); ++Index)
	{
		if (Index > 0)
		{
			Stream << ',';
		}

		Stream << Values[Index];
	}

	std::lock_guard<std::mutex> Lock(SettingsMutex);
	settings_pairs[Key] = Stream.str();
}

void GlueMidi::UpdateInputsConfig(std::string Name, bool bRemove /*= false*/)
{
	std::lock_guard<std::mutex> Lock(SettingsMutex);

	std::vector<std::string> PreviousInputs;

	const auto It = settings_pairs.find("inmidis");

	if (It != settings_pairs.end() && !It->second.empty())
	{
		size_t Start = 0;

		while (Start <= It->second.size())
		{
			const size_t End = It->second.find(',', Start);

			if (End == std::string::npos)
			{
				PreviousInputs.push_back(
					It->second.substr(Start));

				break;
			}

			PreviousInputs.push_back(
				It->second.substr(Start, End - Start));

			Start = End + 1;
		}
	}

	const auto Found = std::find(
		PreviousInputs.begin(),
		PreviousInputs.end(),
		Name);

	if (Found != PreviousInputs.end())
	{
		if (bRemove)
		{
			PreviousInputs.erase(Found);
		}
	}
	else if (!bRemove)
	{
		PreviousInputs.push_back(Name);
	}

	std::ostringstream Stream;

	for (size_t Index = 0;
		Index < PreviousInputs.size();
		++Index)
	{
		if (Index > 0)
		{
			Stream << ',';
		}

		Stream << PreviousInputs[Index];
	}

	settings_pairs["inmidis"] = Stream.str();
}

void GlueMidi::SaveSettings()
{
	std::unordered_map<std::string, std::string> Snapshot;

	{
		std::lock_guard<std::mutex> Lock(SettingsMutex);
		Snapshot = settings_pairs;
	}

	writeAppSettings(iniFileName, Snapshot);
}

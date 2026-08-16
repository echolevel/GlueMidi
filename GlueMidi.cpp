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

//zero padding to make the output hurt my brain less
static std::ostream& Midi7(std::ostream& os, int value)
{
	return os << std::setfill('0') << std::setw(3) << value;
}

static std::ostream& Midi14(std::ostream& os, int value)
{
	return os << std::setfill('0') << std::setw(5) << value;
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

void GlueMidi::ProcessMidiMessageForMonitor(
	const MidiInputUiState& Input,
	const double DeltaTime,
	const std::vector<unsigned char>& Message)
{
	AnimDeltaCounter += DeltaTime;

	if (Message.empty())
	{
		return;
	}

	const uint8_t Status = Message[0];
	const uint8_t Type = Status & 0xF0;
	const int Channel = (Status & 0x0F) + 1;

	const bool IsNote =
		Type == 0x80 ||
		Type == 0x90;

	const bool IsCC = Type == 0xB0;
	const bool IsSysEx = Status == 0xF0;
	const bool IsSystem = Status >= 0xF0 && !IsSysEx;

	const bool IsOtherChannelMessage =
		Status < 0xF0 &&
		!IsNote &&
		!IsCC;

	if ((IsNote && !filterShowNotes) ||
		(IsCC && !filterShowCC) ||
		(IsSysEx && !filterShowSysex) ||
		(IsSystem && !filterShowSys) ||
		(IsOtherChannelMessage && !filterShowOther))
	{
		return;
	}

	size_t RequiredSize = 1;

	if (Status < 0xF0)
	{
		RequiredSize =
			(Type == 0xC0 || Type == 0xD0)
			? 2
			: 3;
	}
	else
	{
		switch (Status)
		{
		case 0xF1:
		case 0xF3:
			RequiredSize = 2;
			break;

		case 0xF2:
			RequiredSize = 3;
			break;

		default:
			RequiredSize = 1;
			break;
		}
	}

	if (Message.size() < RequiredSize)
	{
		return;
	}

	const char* MessageTypeName = "Unknown";

	switch (Type)
	{
	case 0x80:
		MessageTypeName = "Note Off";
		break;

	case 0x90:
		MessageTypeName =
			Message[2] == 0
			? "Note Off"
			: "Note On";
		break;

	case 0xA0:
		MessageTypeName = "Poly AT";
		break;

	case 0xB0:
		MessageTypeName = "CC";
		break;

	case 0xC0:
		MessageTypeName = "Program";
		break;

	case 0xD0:
		MessageTypeName = "Chan AT";
		break;

	case 0xE0:
		MessageTypeName = "Pitch Bend";
		break;

	case 0xF0:
		switch (Status)
		{
		case 0xF0:
			MessageTypeName = "SysEx";
			break;

		case 0xF1:
			MessageTypeName = "MTC Quarter";
			break;

		case 0xF2:
			MessageTypeName = "Song Pos";
			break;

		case 0xF3:
			MessageTypeName = "Song Sel";
			break;

		case 0xF6:
			MessageTypeName = "Tune Req";
			break;

		case 0xF7:
			MessageTypeName = "SysEx End";
			break;

		case 0xF8:
			MessageTypeName = "Clock";
			break;

		case 0xFA:
			MessageTypeName = "Start";
			break;

		case 0xFB:
			MessageTypeName = "Continue";
			break;

		case 0xFC:
			MessageTypeName = "Stop";
			break;

		case 0xFE:
			MessageTypeName = "ActiveSens";
			break;

		case 0xFF:
			MessageTypeName = "Sys Reset";
			break;

		default:
			MessageTypeName = "System";
			break;
		}

		break;
	}

	std::ostringstream Output;
	Output
		<< '['
		<< std::left
		<< std::setfill(' ')
		<< std::setw(12)
		<< MessageTypeName
		<< "] "
		<< std::right
		<< std::setfill('0');

	bool Is14Bit = false;
	int CCNumber = -1;

	if (displayRaw)
	{
		for (const uint8_t Byte : Message)
		{
			Output
				<< std::setfill('0')
				<< std::setw(2)
				<< std::hex
				<< static_cast<int>(Byte)
				<< ' ';
		}
	}
	else
	{
		switch (Type)
		{
		case 0x80:
		case 0x90:
		{
			const int Note = Message[1];
			const int Velocity = Message[2];

			Output
				<< "Ch  " << std::setfill('0') << std::setw(2) << Channel
				<< " Note " << std::setw(3) << Note
				<< " Velo " << std::setw(3) << Velocity;

			break;
		}

		case 0xA0:
			Output
				<< "Ch  " << std::setfill('0') << std::setw(2) << Channel
				<< " Note " << std::setw(3) << static_cast<int>(Message[1])
				<< " Prss " << std::setw(3) << static_cast<int>(Message[2]);
			break;

		case 0xB0:
		{
			CCNumber = Message[1];
			const int CCValue = Message[2];

			Output
				<< "Ch  " << std::setfill('0') << std::setw(2) << Channel
				<< " CCNm " << std::setw(3) << CCNumber;

			if (CCNumber == lastCCnum + 32 &&
				Channel == lastChannel)
			{
				Is14Bit = true;

				const int Value14Bit =
					(lastCCvalue << 7) | CCValue;

				Output
					<< " V14b " << std::setw(5) << Value14Bit;
			}
			else
			{
				Output
					<< " Valu " << std::setw(3) << CCValue;
			}

			lastChannel = Channel;
			lastCCnum = CCNumber;
			lastCCvalue = CCValue;

			break;
		}

		case 0xC0:
			Output
				<< "Ch  " << std::setfill('0') << std::setw(2) << Channel
				<< " Prog " << std::setw(3) << static_cast<int>(Message[1]);
			break;

		case 0xD0:
			Output
				<< "Ch  " << std::setfill('0') << std::setw(2) << Channel
				<< " Prss " << std::setw(3) << static_cast<int>(Message[1]);
			break;

		case 0xE0:
		{
			const int PitchBend =
				(static_cast<int>(Message[2]) << 7) |
				static_cast<int>(Message[1]);

			Output
				<< "Ch  " << std::setfill('0') << std::setw(2) << Channel
				<< " Bend " << std::setw(5) << PitchBend;

			break;
		}

		default:
			for (const uint8_t Byte : Message)
			{
				Output
					<< std::setfill('0')
					<< std::setw(2)
					<< std::hex
					<< static_cast<int>(Byte)
					<< ' ';
			}
			break;
		}
	}

	if (!displayRaw)
	{
		// Don't filter sysex by channel, since sysex messages don't have 'em
		if (filterChannel >= 1 &&
			Status < 0xF0 &&
			Channel != filterChannel)
		{
			return;
		}

		if (filterCC >= 0 &&
			(!IsCC || CCNumber != filterCC))
		{
			return;
		}

		if (filter14bit && !Is14Bit)
		{
			return;
		}
	}

	if (!Input.LogMute)
	{
		bool MatchesInputFilter = isInputEmpty(buf_filter);

		if (!MatchesInputFilter)
		{
			const std::vector<std::string> Filters =
				splitString(
					normaliseString(buf_filter),
					',');

			for (const std::string& Filter : Filters)
			{
				if (Filter.size() >= 3 &&
					partialMatchFilter(
						Filter.c_str(),
						Input.Name))
				{
					MatchesInputFilter = true;
					break;
				}
			}
		}

		if (MatchesInputFilter)
		{
			Log(
				(Output.str() + "\t" + Input.Name)
				.c_str());
		}
	}

	if (AnimDeltaCounter > AnimDeltaThreshold)
	{
		CallAnimate();
		AnimDeltaCounter = 0.0;
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

	ReconcileDesiredMidiState(Snapshot);

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

				// build the absent device list
				const std::vector<std::string> DesiredInputs = GetConfigStringArray("inmidis");

				std::vector<std::string> AbsentInputs;

				for (const std::string& DesiredName : DesiredInputs)
				{
					const bool IsPresent = std::any_of(Snapshot.Inputs.begin(), Snapshot.Inputs.end(),
						[&](const MidiInputUiState& Input)
						{
							return Input.Name == DesiredName;
						});

					if (!IsPresent)
					{
						AbsentInputs.push_back(DesiredName);
					}
				}

				const size_t DisplayInputCount = Snapshot.Inputs.size() + AbsentInputs.size();

				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginListBox("##InputsList", ImVec2(-1, (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y) * DisplayInputCount * 2.0f))) {				

					// Loop the inputs that are present
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
									bSuppressDesiredStateReconcile = false;

									UpdateInputsConfig(Item.Name);
									PendingInputOpenNames.insert(Item.Name);

									MidiWorker_->QueueOpenInput(Item.Index);

									Log(("Input requested: " + Item.Name).c_str());
								}
							}
							else
							{
								if (MidiWorker_)
								{
									UpdateInputsConfig(Item.Name, true); // remove

									PendingInputOpenNames.erase(Item.Name);

									MidiWorker_->QueueCloseInput(Item.Index);
									
									Log(("Input disabled: " + Item.Name).c_str());
								}
							}

							SaveSettings();
						}
						ImGui::PopID();
					}

					// and now loop the devices that were selected but are currently unplugged/absent
					for (const std::string& Name : AbsentInputs)
					{
						ImGui::PushID(Name.c_str());

						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));

						const std::string Label = Name + " (not found)";

						ImGui::Selectable(Label.c_str(), false);

						ImGui::PopStyleColor();

						if (ImGui::BeginPopupContextItem("AbsentInputMenu"))
						{
							if (ImGui::MenuItem("Remove"))
							{
								UpdateInputsConfig(Name, true);
								PendingInputOpenNames.erase(Name);
								SaveSettings();
							}

							ImGui::EndPopup();
						}

						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("This input is enabled in settings but is not currently connected.\nRight-click to remove it.");
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

				const std::string DesiredOutput = GetConfigString("outmidi");

				const bool DesiredOutputPresent =
					DesiredOutput.empty() ||
					std::find(Snapshot.Outputs.begin(), Snapshot.Outputs.end(), DesiredOutput) != Snapshot.Outputs.end();

				const size_t DisplayOutputCount = Snapshot.Outputs.size() + (!DesiredOutput.empty() && !DesiredOutputPresent ? 1 : 0);

				// MIDI out listbox
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginListBox("##outlist", ImVec2(-1, (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y) * DisplayOutputCount * 2.0f)))
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
								if (selected)
								{
									// Explicitly deselect the current output.
									SetConfigString("outmidi", "");
									PendingOutputOpenName.clear();

									MidiWorker_->QueueCloseOutput();

									Log(("Output disabled: " + Snapshot.Outputs[k]).c_str());
								}
								else
								{
									// Select a different output.
									bSuppressDesiredStateReconcile = false;

									SetConfigString("outmidi", Snapshot.Outputs[k]);

									PendingOutputOpenName = Snapshot.Outputs[k];

									MidiWorker_->QueueOpenOutput(k);

									Log(("Output requested: " + Snapshot.Outputs[k]).c_str());
								}

								SaveSettings();
							}							
						}
						ImGui::PopID();
					}

					// and now any absent/unplugged output that's saved in settings
					if (!DesiredOutput.empty() && !DesiredOutputPresent)
					{
						ImGui::PushID("AbsentOutput");

						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));

						const std::string Label = DesiredOutput + " (not found)";

						ImGui::Selectable(Label.c_str(), false);

						ImGui::PopStyleColor();

						if (ImGui::BeginPopupContextItem("AbsentOutputMenu"))
						{
							if (ImGui::MenuItem("Remove"))
							{
								SetConfigString("outmidi", "");
								PendingOutputOpenName.clear();
								SaveSettings();
							}

							ImGui::EndPopup();
						}

						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("This output is selected in settings but is not currently connected.\nRight-click to remove it.");
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
		ImGui::Checkbox("Note", &filterShowNotes);
		ImGui::SameLine();
		ImGui::Checkbox("CC", &filterShowCC);
		ImGui::SameLine();
		ImGui::Checkbox("Other", &filterShowOther);
		ImGui::SetItemTooltip("Pitchbend, aftertouch, etc.");
		ImGui::SameLine();
		ImGui::Checkbox("Sys", &filterShowSys);
		ImGui::SameLine();
		ImGui::Checkbox("SysEx", &filterShowSysex);
		ImGui::SameLine();
		ImGui::Checkbox("Raw", &displayRaw);

		ImGui::SameLine();
		ImGuiInputTextFlags InTextFlags = ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_AutoSelectAll;

		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
		ImGui::InputTextWithHint("##FilterInput", "Filter device names", buf_filter, sizeof(buf_filter), InTextFlags);
		ImGui::SetItemTooltip("Filter Input names with 3 or more matching characters.\nUse ',' to separate multiple filters.\nEmpty filter logs all inputs.");
		
		if (!isInputEmpty(buf_filter))
		{
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_XMARK))
			{
				buf_filter[0] = '\0';
			}
		}		
		
		// channel filter

		ImGui::SameLine();

		static const char* ChannelNames[] =
		{
			"Omni",
			"1",  "2",  "3",  "4",
			"5",  "6",  "7",  "8",
			"9",  "10", "11", "12",
			"13", "14", "15", "16"
		};

		// filterChannel:
		//   0 = Omni
		//   1-16 = MIDI channel

		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);

		int ChannelIndex = filterChannel;

		if (ImGui::Combo(
			"##ChannelFilter",
			&ChannelIndex,
			ChannelNames,
			IM_ARRAYSIZE(ChannelNames)))
		{
			filterChannel = ChannelIndex;
		}

		ImGui::SetItemTooltip("Filter by MIDI channel");

		// channel filter end

		
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

void GlueMidi::releaseMidiPorts()
{
	if (!MidiWorker_)
	{
		return;
	}

	bSuppressDesiredStateReconcile = true;

	PendingInputOpenNames.clear();
	PendingOutputOpenName.clear();

	MidiWorker_->QueueReleaseAll();
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

void GlueMidi::ReconcileDesiredMidiState(const MidiPortUiSnapshot& Snapshot)
{	
	if (!MidiWorker_ || bSuppressDesiredStateReconcile)
	{
		return;
	}

	const std::vector<std::string> DesiredInputs = GetConfigStringArray("inmidis");

	// Forget pending opens for devices that are no longer present.
	for (auto It = PendingInputOpenNames.begin(); It != PendingInputOpenNames.end();)
	{
		const bool IsPresent = std::any_of(Snapshot.Inputs.begin(), Snapshot.Inputs.end(),
			[&](const MidiInputUiState& Input)
			{
				return Input.Name == *It;
			});

		if (!IsPresent)
		{
			It = PendingInputOpenNames.erase(It);
		}
		else
		{
			++It;
		}
	}

	// check against the settings list
	for (const MidiInputUiState& Input : Snapshot.Inputs)
	{
		const bool IsDesired = std::find(DesiredInputs.begin(), DesiredInputs.end(), Input.Name) != DesiredInputs.end();

		if (Input.Active)
		{
			PendingInputOpenNames.erase(Input.Name);
			continue;
		}

		if (IsDesired &&
			!PendingInputOpenNames.contains(Input.Name))
		{
			PendingInputOpenNames.insert(Input.Name);
			MidiWorker_->QueueOpenInput(Input.Index);
		}
	}


	const std::string DesiredOutput = GetConfigString("outmidi");

	bool DesiredOutputPresent = false;
	bool DesiredOutputOpen = false;
	int DesiredOutputIndex = -1;

	// same for output
	for (int Index = 0; Index < static_cast<int>(Snapshot.Outputs.size()); ++Index)
	{
		if (Snapshot.Outputs[Index] != DesiredOutput)
		{
			continue;
		}

		DesiredOutputPresent = true;
		DesiredOutputIndex = Index;

		DesiredOutputOpen = Snapshot.OutputOpen && Snapshot.CurrentOutputIndex == Index;

		break;
	}

	if (DesiredOutputOpen)
	{
		PendingOutputOpenName.clear();
	}
	else if (!DesiredOutputPresent)
	{
		// Allow another open attempt if it reappears later.
		PendingOutputOpenName.clear();
	}
	else if (!DesiredOutput.empty() &&
		PendingOutputOpenName != DesiredOutput)
	{
		PendingOutputOpenName = DesiredOutput;

		MidiWorker_->QueueOpenOutput(
			DesiredOutputIndex);
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

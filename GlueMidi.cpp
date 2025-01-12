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
}

GlueMidi::~GlueMidi()
{

}

void GlueMidi::Update()
{
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
			releaseMidiPorts(inputStatus, outputStatus);
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
				if (ImGui::BeginListBox("##InputsList")) {
				
					// Show the MIDI inputs list box.
					// Clicking an item will toggle that input open or closed.
					// Multiple inputs can be open. Closed inputs should be removed
					// from ActiveMidiInNames.

					for (int i = 0; i < MidiInNames.size(); i++)
					{
						ImGui::PushID(i);
						bool selected = false;
						
						for (size_t a = 0; a < ActiveMidiPortNumbers.size(); a++)
						{
							if (i == ActiveMidiPortNumbers[a])
							{
								selected = true;
							}
						}

						if (ImGui::Selectable(MidiInNames[i].c_str(), &selected))
						{
							if (selected)
							{
								openMidiInPort(i);
								ActiveMidiPortNumbers.push_back(i);
								ActiveMidiInNames.push_back(MidiInNames[i]);
								SetConfigStringArray("inmidis", ActiveMidiInNames);
								Log((MidiInNames[i] + " OPENED").c_str());
							}
							else
							{
								// It's been deselected, so we have to remove it from
								// ActiveMidiInNames, close the port, and write the config array
								for (size_t m = 0; m < midiInputs.size(); m++)
								{
									if (midiInputs[m]->getPortName(i).length() > 0)
									{
										midiInputs[m]->closePort();
										Log((midiInputs[m]->getPortName(i) + " CLOSED").c_str());
									}									
								}

								for (size_t n = 0; n < ActiveMidiInNames.size(); n++)
								{
									if (ActiveMidiInNames[n] == MidiInNames[i])
									{
										remove(ActiveMidiInNames, n);
									}
								}

								for (size_t q = 0; q < ActiveMidiPortNumbers.size(); q++)
								{
									if (ActiveMidiPortNumbers[q] == i)
									{
										remove(ActiveMidiPortNumbers, q);
									}
								}
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
				if ((midiout != nullptr) && !midiout->isPortOpen())
				{
					snprintf(outputStatus, 32, "Inactive");
				}

				// MIDI out listbox
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginListBox("##outlist"))
				{
					// MIDI out selectables
					for (int k = 0; k < MidiOutNames.size(); k++)
					{
						ImGui::PushID(k);
						bool selected = MidiOutIndex == k;

						if (ImGui::Selectable((MidiOutNames[k] + "##2").c_str(), &selected))
						{
							MidiOutIndex = k;
							midiout->closePort();
							openMidiOutPort(k);
							SetConfigString("outmidi", MidiOutNames[MidiOutIndex]);
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
		ImGui::Text("Filter:");

		ImGui::SameLine();

		if (!MidiInNames.empty())
		{
			const char* filter_combo_preview_value = MidiInNames[FilterPortIndex].c_str();;
			ImGui::SetNextItemWidth(200.f);

			ImGui::BeginDisabled(!FilterByPort);
			if (ImGui::BeginCombo("##MonitorPortFilter", filter_combo_preview_value, 0))
			{
				for (int i = 0; i < MidiInNames.size(); i++)
				{
					const bool is_selected = (FilterPortIndex == i);

					if (ImGui::Selectable(MidiInNames[i].c_str(), is_selected))
					{
						FilterPortIndex = i;
						filter_combo_preview_value = MidiInNames[i].c_str();
					}
					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}

				}
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			static bool bPortfilter = FilterByPort;
			if (ImGui::Checkbox("Port", &bPortfilter))
			{
				FilterByPort = bPortfilter;
			}
		}
				
		
		ImGui::Text("Show");
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

	// Synchronize GLFW window size with ImGui window
	//SyncWindowSizeWithImGui(window);

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
	if (!midiin || !midiout)
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
			//return -1;
		}
	}

	if (midiin)
	{
		MidiInNames.clear();		
		midiInputs.clear(); 

		unsigned int nPorts = midiin->getPortCount();
		std::string portName;

		for (unsigned int i = 0; i < nPorts; i++)
		{
			try
			{
				portName = midiin->getPortName(i);
			}
			catch (RtMidiError& error)
			{
				std::cerr << "RtMidiError: ";
				error.printMessage();
				Log(error.getMessage().c_str());
				return -1;
			}
			std::cout << " Input Port #" << i + 1 << ": " << portName << '\n';

			// Store the name after stripping the index number, so we can compare 
			// with devices of the same name (but possibly a different index) on 
			// next startup.
			std::string nameTrimmed = portName.substr(0, portName.find_last_of(' '));
			MidiInNames.push_back(nameTrimmed);	
			MidiPortNumbers.push_back(i); // This is more important; the name is just for comparison and display
			Log(nameTrimmed.c_str());				
		}

	}
	

	if (midiout)
	{
		// Clear old names
		MidiOutNames.clear();

		// Check outputs.
		unsigned int nPorts = midiout->getPortCount();
		std::string portName;
		std::cout << "\nThere are " << nPorts << " MIDI output ports available.\n";
		for (unsigned int i = 0; i < nPorts; i++) {
			try {
				portName = midiout->getPortName(i);
			}
			catch (RtMidiError& error) {
				error.printMessage();
				Log(error.getMessage().c_str());
				return -1;
			}
			std::cout << "  Output Port #" << i + 1 << ": " << portName << '\n';
			// Store the name after stripping the index number, so we can compare 
			// with devices of the same name (but possibly a different index) on 
			// next startup.
			std::string nameTrimmed = portName.substr(0, portName.find_last_of(' '));
			MidiOutNames.push_back(nameTrimmed);
			Log(nameTrimmed.c_str());
		}
		std::cout << '\n';
	}

	return 1;
}

// We need to use a static var for the main GlueMidi instance because it's for some reason 
// impossible to pass RtMidi our callback function when it's a member of a class, and I 
// couldn't get it to work as a lambda either.
static GlueMidi* globalInstance = nullptr;

static void midiInCallback(double deltatime, std::vector<unsigned char>* message, void* userData )
{
	const char* InputName = static_cast<const char*>(userData);

	if (globalInstance == nullptr)
	{
		return;
	}

	if (message->size() < 1)
	{
		return;
	}

	int ccNum = 0;
	int ccChan = 0;
	int ccValue = 0;
	bool is14bit = false;
	int value14bit = 0;

	std::stringstream finalhexout;

	// Is this a sysex message?
	if (((int)message->at(0) == 0xF0) && (message->size() >= 14) && !globalInstance->displayRaw)
	{
		std::vector<unsigned char>::iterator it = message->begin();

		globalInstance->Log("Received config dump from Fader3");

	}

	// Is this a control message status byte? Check if MSB is 0B
	else if ((((int)message->at(0) >> 4) == 0x0b) && !globalInstance->displayRaw)
	{
		ccNum = (int)message->at(1);
		ccChan = ((int)message->at(0) & 0x0F) + 1;
		ccValue = (int)message->at(2);

		is14bit = false;
		value14bit = 0;
		// If this CC number is equal to the last one + 32, and the channel is the same, it's 14-bit o'clock
		if ((ccNum == (globalInstance->lastCCnum + 32)) && (ccChan == globalInstance->lastChannel))
		{
			is14bit = true;
			value14bit = (globalInstance->lastCCvalue << 7) | ccValue;
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
		globalInstance->lastChannel = ccChan;
		globalInstance->lastCCnum = ccNum;
		globalInstance->lastCCvalue = ccValue;
	}

	// It's not CC or sysex so just display raw bytes
	else for (auto it = message->begin(); it != message->end(); it++)
	{
		finalhexout << std::setfill('0') << std::setw(sizeof(char) * 2) << std::hex << int(*it) << " ";
	}

	// Always display raw bytes if filter enabled
	if (!globalInstance->displayRaw)
	{
		// Die if channel filter is enabled and this doesn't match
		if (globalInstance->filterChannel >= 1 && ccChan != globalInstance->filterChannel)
			return;

		// Die if CC filter is enabled and this doesn't match
		if (globalInstance->filterCC >= 0 && ccNum != globalInstance->filterCC)
			return;

		// Die if this is a 7bit value but the 14-bit filter is enabled
		if (globalInstance->filter14bit && !is14bit)
			return;
	}
	
	globalInstance->CallAnimate();

	globalInstance->Log((finalhexout.str() + + "\t" + InputName).c_str());

	globalInstance->SendMessageOnPort(message, globalInstance->midiout);
}

int GlueMidi::openMidiInPort(int InIndex)
{	
	try
	{
		// Make sure our GlueMidi instance will be accessible in the callback
		if (globalInstance == nullptr)
		{
			globalInstance = this;
		}
		// Create the instance
		auto thisInput = std::make_unique<RtMidiIn>();
		thisInput->setCallback(midiInCallback, (void*)MidiInNames[InIndex].c_str());
		thisInput->openPort(InIndex);		
		thisInput->ignoreTypes(false, true, true);

		midiInputs.push_back(std::move(thisInput));

		// TO DO - status strings per input
		snprintf(inputStatus, 32, "Active");
		return 1;

		Log(MidiInNames[InIndex].c_str());
		
	}
	catch (RtMidiError& error) {
		error.printMessage();
		Log(error.getMessage().c_str());
		snprintf(inputStatus, 32, "Inactive");
		return 0;
	}

	return 0;
}


int GlueMidi::openMidiOutPort(int OutIndex)
{
	if (midiout->isPortOpen())
		midiout->closePort();

	try
	{
		midiout->openPort(OutIndex, MidiOutNames[OutIndex]);
		Log(MidiOutNames[OutIndex].c_str());
		snprintf(outputStatus, 32, "Active");
		return 1;
	}
	catch (RtMidiError& error) {
		error.printMessage();
		Log(error.getMessage().c_str());
		snprintf(outputStatus, 32, "Inactive");
		return 0;
	}

	return 0;
}

void GlueMidi::releaseMidiPorts(char* inputStatus, char* outputStatus)
{
	if (midiin && midiin->isPortOpen())
	{
		midiin->closePort();
		sprintf_s(inputStatus, 32, "Inactive");
	}

	if (midiout && midiout->isPortOpen())
	{
		midiout->closePort();
		sprintf_s(outputStatus, 32, "Inactive");
	}

	Log("MIDI ports closed");
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

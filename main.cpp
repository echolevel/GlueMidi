
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <tchar.h>
#include <windowsx.h>
#include <locale>
#include <codecvt>
#define __WINDOWS_MM__
#include "GlueMidi.h"


// Data
static ID3D11Device* g_pd3dDevice = NULL;
static ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
static IDXGISwapChain* g_pSwapChain = NULL;
static ID3D11RenderTargetView* g_mainRenderTargetView = NULL;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HWND g_hwnd;

bool maindone = false;

NOTIFYICONDATA nid = {}; // Tray icon data

static bool ImGuiContextCreated = false;
static bool CloseToTray = false;

HMENU hTrayMenu;

std::vector<HICON> iconFrames;
int currentIconFrame = 0;

#define NID_ICOID 1001   // Unique identifier for the tray icon
#define NID_CBMSG (WM_USER + 1)   // Custom message for tray events

#define IDI_ICON1 101  // Define the ID for the icon
#define IDI_ICON2 102  // Define the ID for the icon
#define IDI_ICON3 103  // Define the ID for the icon
#define IDI_ICON4 104  // Define the ID for the icon
#define IDI_ICON5 105  // Define the ID for the icon
#define IDI_ICON6 106  // Define the ID for the icon

// Prepend/append e.g. debugLog(("Sometext: " + astring + "\n").c_str());
void debugLog(std::string message) {	
	OutputDebugStringA(message.c_str());
}

void LoadIcons(HINSTANCE hInstance) {
	// Load the icons for animation (you should have a series of icon files)	


	HICON hIcon = (HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	if (!hIcon) {
		DWORD error = GetLastError();
		wchar_t errorMessage[256];
		swprintf_s(errorMessage, L"Failed to load icon. Error code: %lu", error);
		MessageBox(NULL, errorMessage, L"Error", MB_OK | MB_ICONERROR);
	}

	iconFrames.push_back((HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)));
	iconFrames.push_back((HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON2)));
	iconFrames.push_back((HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON3)));
	iconFrames.push_back((HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON4)));
	iconFrames.push_back((HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON5)));
}

void CreateTrayContextMenu()
{
	hTrayMenu = CreatePopupMenu();
	AppendMenu(hTrayMenu, MF_STRING, 1, L"Refresh Ports");
	AppendMenu(hTrayMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hTrayMenu, MF_STRING, 2, L"Quit");

}

void ShowTrayContextMenu(HWND hwnd, POINT pt)
{
	TrackPopupMenu(hTrayMenu, TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
	PostMessage(hwnd, WM_NULL, 0, 0); // to prevent menu from staying open after clicking
}

void AdvanceTrayIconFrame()
{
	currentIconFrame = (currentIconFrame + 1) % iconFrames.size();  // Loop through icon frames
	
	nid.hIcon = iconFrames[currentIconFrame];  // Set the current icon
	Shell_NotifyIcon(NIM_MODIFY, &nid);  // Modify the tray icon

	// Update the toolbar and taskbar icons
	SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)iconFrames[currentIconFrame]);
	SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconFrames[currentIconFrame]);
		
	// Optionally update the tooltip with the current frame info
	//std::wstring tooltip = L"Frame " + std::to_wstring(currentIconFrame + 1);
	//wcscpy_s(nid.szTip, tooltip.c_str());
	//Shell_NotifyIcon(NIM_MODIFY, &nid);
}

// Function to minimize the window to the system tray
void minimizeToTray(HWND hwnd) {
	// Hide the window (so it doesn't appear in the taskbar)
	ShowWindow(hwnd, SW_HIDE);
}

// Function to restore the window from the system tray
void restoreFromTray(HWND hwnd) {
	// Ensure the window is not minimized and is shown
	ShowWindow(hwnd, SW_RESTORE);          // Restore the window if minimized
	SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);  // Ensure correct position

	// Bring the window to the foreground (optional, if you want the window to come to the top)
	SetForegroundWindow(hwnd);

	// Remove the tray icon
	//Shell_NotifyIcon(NIM_DELETE, &nid);
}



float GetDpiScalingFactor(HWND hwnd)
{
	UINT dpi = GetDpiForWindow(hwnd);  // Get the DPI for the current window
	float scaleFactor = (float)dpi / 96.0f;  // Convert to scaling factor
	return scaleFactor;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // Enforce DPI awareness
	
	GlueMidi* gluemidi;

	try
	{
		 gluemidi = new GlueMidi(AdvanceTrayIconFrame);
	}
	catch (RtMidiError& error) {
		error.printMessage();
		debugLog(error.getMessage());
		exit(EXIT_FAILURE);
	}
	

	// Get the monitor's dimensions so we can start the app bang in the middle of it,
	// in case there are no previously stored settings
	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);
	// Calculate the centered position
	int wPosX = (screenWidth - gluemidi->width) / 2;
	int wPosY = (screenHeight - gluemidi->height) / 2;

	if (gluemidi->settings_were_loaded)
	{
		// Try to get width and height	
		gluemidi->width = gluemidi->GetConfigInt("width");
		gluemidi->height = gluemidi->GetConfigInt("height");

		// Now look for previously stored position settings
		wPosX = gluemidi->GetConfigInt("xpos");
		wPosY = gluemidi->GetConfigInt("ypos");

		// Store this so that Win32 callback can check it
		CloseToTray = (bool)gluemidi->GetConfigInt("closetotray");
	}

	



	

	

	if (gluemidi->refreshMidiPorts() < 1)
	{
		return -1;
	}

	if (gluemidi->settings_were_loaded)
	{
		// Attempt to open the previously used MIDI device ports.
		// Note that we copy the config array first, and don't remove any ports that
		// we don't find in the current MidiInNames. Maybe you'll plug it in next time,
		std::vector<std::string> PrevMidiIns = gluemidi->GetConfigStringArray("inmidis");
		gluemidi->ActiveMidiInNames = gluemidi->GetConfigStringArray("inmidis");
		if (gluemidi->ActiveMidiInNames.size() > 0)
		{
			for (size_t t = 0; t < gluemidi->MidiInNames.size(); t++)
			{
				// For every name, check if it should be opened
				for (size_t a = 0; a < gluemidi->ActiveMidiInNames.size(); a++)
				{
					std::string CompareName = gluemidi->ActiveMidiInNames[a];

					if (CompareName == gluemidi->MidiInNames[t])
					{
						gluemidi->openMidiInPort(t);
						gluemidi->ActiveMidiPortNumbers.push_back(t);
					}
				}

			}
		}



		std::string PrevMidiOut = gluemidi->GetConfigString("outmidi");

		//
		// TO DO - attempt to restore previously opened input ports
		//

		for (int i = 0; i < gluemidi->MidiOutNames.size(); i++)
		{
			if (gluemidi->MidiOutNames[i] == PrevMidiOut)
			{
				gluemidi->MidiOutIndex = i;
				gluemidi->openMidiOutPort(i);
			}
		}
	}
	

	

	
	


	// Create application window
	WNDCLASSEX wc = { sizeof(WNDCLASSEX), 
		CS_CLASSDC, 
		WndProc, 
		0L, 
		0L, 
		GetModuleHandle(NULL), 
		NULL,
		NULL, 
		NULL, 
		NULL, 
		_T("GlueMidi"), 
		NULL 
	};

	// Check for dark mode so we can load a version of the icon that'll be visible
	
	// Open the registry to check the system theme
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
	{
		return NULL;  // Default icon if registry key fails
	}
	DWORD dwValue = 0;
	DWORD dwSize = sizeof(dwValue);

	// Read the AppsUseLightTheme value to determine if system is using light or dark mode
	if (RegQueryValueEx(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&dwValue, &dwSize) != ERROR_SUCCESS)
	{
		RegCloseKey(hKey);
		return NULL;
	}
	RegCloseKey(hKey);

	if (dwValue == 0) // dark mode
	{
		wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON5));
		wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON5));
	}
	else // Light mode
	{
		wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON6));
		wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON6));
	}

	

	RegisterClassEx(&wc);
	HWND g_hwnd = CreateWindow(wc.lpszClassName, _T("GlueMidi"),
		WS_OVERLAPPEDWINDOW,
		1280,
		400,
		gluemidi->width,
		gluemidi->height,
		NULL, NULL, wc.hInstance, NULL);

	// Load icons for animation
	LoadIcons(hInstance);

	// Initialize Direct3D
	if (!CreateDeviceD3D(g_hwnd))
	{
		CleanupDeviceD3D();
		UnregisterClass(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	ShowWindow(g_hwnd, SW_SHOWDEFAULT);
	UpdateWindow(g_hwnd);

	// Update wpos, either with saved coords or the middle of the screen
	SetWindowPos(g_hwnd, NULL, wPosX, wPosY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

	// Set up the tray icon regardless of whether app is minimised
	// Initialize the NOTIFYICONDATA structure
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = g_hwnd;
	nid.uID = NID_ICOID;
	nid.uVersion = NOTIFYICON_VERSION;
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = NID_CBMSG;

	// Load the icon for the tray
	//nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // Use default icon for now
	nid.hIcon = iconFrames[0];

	// Set the tooltip for the tray icon
	wcscpy_s(nid.szTip, L"GlueMidi");

	// Add the tray icon
	Shell_NotifyIcon(NIM_ADD, &nid);

	CreateTrayContextMenu();

	// tray icon stuff end

	// Has the user enabled 'start minimised to tray'?	
	if (gluemidi->GetConfigInt("startintray") > 0)
	{
		minimizeToTray(g_hwnd);
	}

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	ImGuiContextCreated = true;
	// Disable imgui's ini, since we're handling window size/pos ourselves and the true values aren't written to it
	io.IniFilename = NULL;

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	SetBessTheme();

	gluemidi->setupImGuiFonts();

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	

	// Setup Dear ImGui style
	SetBessTheme();

	gluemidi->setupImGuiFonts();
	
	std::unordered_map<std::string, std::string> configPairs;

	bool done = gluemidi->bDone;
	// Main loop	
	while (!gluemidi->bDone)
	{
		MSG msg;
		while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (gluemidi->bDone)
			break;

		// Do most of the work
		gluemidi->Update();

		
		if (g_hwnd)
		{
			RECT rect;
			GetWindowRect(g_hwnd, &rect);
			gluemidi->wposx = rect.left;
			gluemidi->wposy = rect.top;
			gluemidi->width = rect.right - rect.left;
			gluemidi->height = rect.bottom - rect.top;
		}


		// Rendering
		ImGui::Render();
		const float clear_color[4] = { 0.f, 0.f, 0.f, 1.00f };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_pSwapChain->Present(1, 0); // Present with vsync

		if (maindone)
		{
			gluemidi->bDone = true;
		}
	}

	

	
	// CLEANUP


	
	gluemidi->SetConfigInt("xpos", gluemidi->wposx);
	gluemidi->SetConfigInt("ypos", gluemidi->wposy);
	gluemidi->SetConfigInt("width", gluemidi->width);
	gluemidi->SetConfigInt("height", gluemidi->height);
	
	HMONITOR hMonitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
	if (hMonitor)
	{
		MONITORINFOEX monitorInfo;
		monitorInfo.cbSize = sizeof(MONITORINFOEX);

		if (GetMonitorInfo(hMonitor, &monitorInfo))
		{			
			std::wstring mon = monitorInfo.szDevice;
			gluemidi->SetConfigString("monitor", std::string(mon.begin(), mon.end()));
		}
	}
	
	// TO DO - make this all work. Testing ser/deser with the full array of names for now
	std::vector<std::string> activeInputs;
	
	gluemidi->SetConfigStringArray("inmidis", gluemidi->ActiveMidiInNames);

	gluemidi->SetConfigString("outmidi", gluemidi->MidiOutNames[gluemidi->MidiOutIndex]);
	gluemidi->SaveSettings();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	
	CleanupDeviceD3D();
	DestroyWindow(g_hwnd);
	UnregisterClass(wc.lpszClassName, wc.hInstance);

	gluemidi->midiin->closePort();
	gluemidi->midiout->closePort();
	delete gluemidi->midiin;
	delete gluemidi->midiout;
	delete gluemidi;

	for (HICON icon : iconFrames)
	{
		DestroyIcon(icon);
	}

	return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
		D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, NULL, &g_pd3dDeviceContext);
	if (FAILED(hr))
		return false;

	CreateRenderTarget();
	return true;
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
	if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
	if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
	pBackBuffer->Release();
}

void CleanupRenderTarget()
{
	if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
	{
		return true;
	}

	switch (msg)
	{

	
	case WM_LBUTTONDOWN: // LMB pressed on top bar
		POINT cursorPos;
		GetCursorPos(&cursorPos);
		ScreenToClient(hWnd, &cursorPos); // screen coords to client coords
		RECT windowRect;
		GetClientRect(hWnd, &windowRect);

		if (cursorPos.y >= windowRect.top && cursorPos.y <= (windowRect.top + 20)
			&& cursorPos.x >= windowRect.left && cursorPos.x <= windowRect.right)
		{
			SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); // Send drag message
		}
		return 0;


	case WM_NCHITTEST:
	{
		// Get the mouse position relative to the window
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hWnd, &pt);

		RECT rect;
		GetClientRect(hWnd, &rect);
		ImGui::GetIO().DisplaySize = ImVec2((float)(rect.right - rect.left), (float)(rect.bottom - rect.top));

		const int borderWidth = 8;  // Thickness of the resize area

		// Determine the hit-test result based on the mouse position
		if (pt.x < borderWidth && pt.y < borderWidth) return HTTOPLEFT;
		if (pt.x > rect.right - borderWidth && pt.y < borderWidth) return HTTOPRIGHT;
		if (pt.x < borderWidth && pt.y > rect.bottom - borderWidth) return HTBOTTOMLEFT;
		if (pt.x > rect.right - borderWidth && pt.y > rect.bottom - borderWidth) return HTBOTTOMRIGHT;
		if (pt.x < borderWidth) return HTLEFT;
		if (pt.x > rect.right - borderWidth) return HTRIGHT;
		if (pt.y < borderWidth) return HTTOP;
		if (pt.y > rect.bottom - borderWidth) return HTBOTTOM;

		// Default to client area
		return HTCLIENT;
	}
	case WM_SIZE:
		if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED)
		{
			// Get the new window dimensions
			UINT width = LOWORD(lParam);
			UINT height = HIWORD(lParam);

			CleanupRenderTarget();
			g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();

			if (ImGuiContextCreated)
			{
				ImGuiIO& io = ImGui::GetIO();
				io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));				
			}			
			
		}
		return 0;		

	case WM_SYSCOMMAND:		
		if (wParam == SC_MINIMIZE)
		{
			minimizeToTray(hWnd);
			return 0;
		}
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;

	case WM_CLOSE:
		if (CloseToTray)
		{
			minimizeToTray(hWnd);
			return 0;
		}
		else
		{
			maindone = true;
			DestroyWindow(hWnd);
		}
		break;

	case NID_CBMSG:  // Custom message for tray icon events
		if (lParam == WM_LBUTTONUP) {  // Left double-click
			restoreFromTray(hWnd);
		}
		if (lParam == WM_RBUTTONUP)
		{
			POINT pt;
			GetCursorPos(&pt);
			ShowTrayContextMenu(hWnd, pt);
		}
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
			case 1: 
				MessageBox(hWnd, L"Refresh Ports", L"Info", MB_OK);
				break;
			case 2:
				Shell_NotifyIcon(NIM_DELETE, &nid);
				PostMessage(hWnd, WM_CLOSE, 0, 0);
				break;

		}

	case WM_DESTROY:
		// Remove the tray icon when the window is destroyed
		Shell_NotifyIcon(NIM_DELETE, &nid);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}


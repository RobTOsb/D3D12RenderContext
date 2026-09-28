#include <stdio.h>

#include "Window.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "imgui.h"

#include <stdexcept>

#include <fmtlog.h>

Window::Window(const WindowDesc &desc)
{
#if defined(_WIN32)
	auto MakeProcessDpiAware = []()
	{
		HMODULE user32 = LoadLibraryA("user32.dll");
		if (user32)
		{
			typedef BOOL(WINAPI * SetProcessDpiAwarenessContext_t)(HANDLE);
			auto fn = (SetProcessDpiAwarenessContext_t) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
			if (fn)
			{
				fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
				FreeLibrary(user32);
				return;
			}
			FreeLibrary(user32);
		}

		HMODULE shcore = LoadLibraryA("shcore.dll");
		if (shcore)
		{
			typedef HRESULT(WINAPI * SetProcessDpiAwareness_t)(int);
			auto fn2 = (SetProcessDpiAwareness_t) GetProcAddress(shcore, "SetProcessDpiAwareness");
			if (fn2)
			{
				fn2(2); // PROCESS_PER_MONITOR_DPI_AWARE
				FreeLibrary(shcore);
				return;
			}
			FreeLibrary(shcore);
		}

		HMODULE user32b = LoadLibraryA("user32.dll");
		if (user32b)
		{
			typedef BOOL(WINAPI * SetProcessDPIAware_t)();
			auto fn3 = (SetProcessDPIAware_t) GetProcAddress(user32b, "SetProcessDPIAware");
			if (fn3)
			{
				fn3();
			}
			FreeLibrary(user32b);
		}
	};

	MakeProcessDpiAware();
#endif

	WNDCLASSA wc = {};
	wc.lpfnWndProc = Window::WindowProc;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = desc.title;

	RegisterClassA(&wc);

	DWORD dwStyle = WS_OVERLAPPEDWINDOW;
	int windowWidth = desc.width;
	int windowHeight = desc.height;

	if (desc.fullscreen)
	{
		dwStyle = WS_POPUP;
		windowWidth = GetSystemMetrics(SM_CXSCREEN);
		windowHeight = GetSystemMetrics(SM_CYSCREEN);
	}

	windowHandle_ = CreateWindowExA(0,
									desc.title,
									desc.title,
									dwStyle,
									0,
									0,
									windowWidth,
									windowHeight,
									nullptr,
									nullptr,
									wc.hInstance,
									this);

	if (!windowHandle_)
	{
		throw std::runtime_error("Failed to create window.");
	}

	ShowWindow(static_cast<HWND>(windowHandle_), SW_SHOW);
	UpdateWindow(static_cast<HWND>(windowHandle_));

	RECT rect;
	GetClientRect(static_cast<HWND>(windowHandle_), &rect);
	width_ = rect.right - rect.left;
	height_ = rect.bottom - rect.top;

	logi("Window successfully created: {} ({}x{})", desc.title, width_, height_);
}

Window::~Window()
{
	if (windowHandle_)
	{
		DestroyWindow(static_cast<HWND>(windowHandle_));
		windowHandle_ = nullptr;
	}

	logi("Window successfully destroyed.");
}

void *Window::GetNativeWindow() const
{
	return windowHandle_;
}

bool Window::ProcessEvents()
{
	// Clear per-frame toggle state at the start of each event-pump.
	memset(keyWasToggled_, 0, sizeof(keyWasToggled_));

	MSG msg;
	while (true)
	{
		if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				return false;
			}

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			return true;
		}
	}
}

long long __stdcall Window::WindowProc(HWND__ *hwnd, uint32_t msg, uintptr_t wParam, intptr_t lParam)
{
	Window *self = nullptr;

	if (msg == WM_CREATE)
	{
		CREATESTRUCTA *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
		if (cs)
		{
			self = reinterpret_cast<Window *>(cs->lpCreateParams);
			SetWindowLongPtrA(reinterpret_cast<HWND>(hwnd), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		}
	}
	else
	{
		self = reinterpret_cast<Window *>(GetWindowLongPtrA(reinterpret_cast<HWND>(hwnd), GWLP_USERDATA));
	}

	if(ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
	{
		return 0;
	}

	// ImGui saw the event above but doesn't consume mouse/keyboard messages itself (so the app can
	// still see e.g. window drag); if it wants the input this frame, don't also feed it to the camera.
	ImGuiIO *io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
	const bool imguiWantsMouse = io && io->WantCaptureMouse;
	const bool imguiWantsKeyboard = io && io->WantCaptureKeyboard;

	switch (msg)
	{
		case WM_CREATE:
		{
			return 0;
		}
		case WM_KEYDOWN:
		{
			if (wParam == VK_ESCAPE)
			{
				DestroyWindow(reinterpret_cast<HWND>(hwnd));
			}
			// Bit 30 of lParam is 1 for key-repeat messages; only record a
			// toggle on the initial key-down (non-repeat) transition.
			if (self && !imguiWantsKeyboard && !(lParam & 0x40000000) && wParam < 256)
			{
				self->keyWasToggled_[wParam] = true;
			}
			return 0;
		}
		case WM_CLOSE:
		{
			DestroyWindow(reinterpret_cast<HWND>(hwnd));
			return 0;
		}
		case WM_DPICHANGED:
		{
			int dpiX = LOWORD(wParam);
			int dpiY = HIWORD(wParam);
			RECT *suggested = reinterpret_cast<RECT *>(lParam);
			if (suggested)
			{
				SetWindowPos(reinterpret_cast<HWND>(hwnd),
							 nullptr,
							 suggested->left,
							 suggested->top,
							 suggested->right - suggested->left,
							 suggested->bottom - suggested->top,
							 SWP_NOZORDER | SWP_NOACTIVATE);
			}

			if (self)
			{
				RECT rect;
				GetClientRect(reinterpret_cast<HWND>(hwnd), &rect);
				self->width_ = rect.right - rect.left;
				self->height_ = rect.bottom - rect.top;
				logi("WM_DPICHANGED: new DPI {}x{}, scale {:.2f}", dpiX, dpiY, static_cast<float>(dpiX) / 96.0f);
			}

			return 0;
		}
		case WM_SIZE:
		{
			if (self && wParam != SIZE_MINIMIZED)
			{
				uint32_t newWidth = LOWORD(lParam);
				uint32_t newHeight = HIWORD(lParam);
				if (newWidth > 0 && newHeight > 0 &&
					(newWidth != self->width_ || newHeight != self->height_))
				{
					self->width_ = newWidth;
					self->height_ = newHeight;
					if (self->resizeCallback_)
						self->resizeCallback_(self->resizeCallbackUserData_, newWidth, newHeight);
				}
			}
			return 0;
		}
		case WM_MOUSEWHEEL:
		{
			if (self && !imguiWantsMouse)
			{
				short delta = GET_WHEEL_DELTA_WPARAM(wParam);
				self->mouseWheelDelta_ += delta;
			}
			return 0;
		}
		case WM_MOUSEMOVE:
		{
			// Always tracked (not gated on WantCaptureMouse) so drag deltas stay continuous once a
			// drag that started over the scene continues over a UI window.
			if (self)
			{
				self->currentMouseX_ = GET_X_LPARAM(lParam);
				self->currentMouseY_ = GET_Y_LPARAM(lParam);
			}
			return 0;
		}
		case WM_LBUTTONDOWN:
		{
			if (self && !imguiWantsMouse)
			{
				self->isLeftMouseButtonDown_ = true;
				self->lastMouseX_ = self->currentMouseX_;
				self->lastMouseY_ = self->currentMouseY_;
			}
			return 0;
		}
		case WM_RBUTTONDOWN:
		{
			if (self && !imguiWantsMouse)
			{
				self->isRightMouseButtonDown_ = true;
				self->lastMouseX_ = self->currentMouseX_;
				self->lastMouseY_ = self->currentMouseY_;
			}
			return 0;
		}
		case WM_LBUTTONUP:
		{
			if (self)
			{
				self->isLeftMouseButtonDown_ = false;
			}
			return 0;
		}
		case WM_RBUTTONUP:
		{
			if (self)
			{
				self->isRightMouseButtonDown_ = false;
			}
			return 0;
		}
		case WM_DESTROY:
		{
			PostQuitMessage(0);
			return 0;
		}
		default:
		{
			break;
		}
	}

	return DefWindowProcA(reinterpret_cast<HWND>(hwnd), msg, wParam, lParam);
}

bool Window::WasKeyPressed(uint32_t virtualKeyCode) const
{
	return (GetKeyState(virtualKeyCode) & 0x8000) != 0;
}

bool Window::WasKeyToggled(uint32_t virtualKeyCode) const
{
	if (virtualKeyCode >= 256)
		return false;
	return keyWasToggled_[virtualKeyCode];
}

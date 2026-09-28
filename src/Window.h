#pragma once

#include <cstdint>

struct HWND__;

struct WindowDesc
{
	const char *title = "D3D12 Renderer";
	uint32_t width = 1920;
	uint32_t height = 1080;
	bool fullscreen = false;
};

class Window
{
public:
	Window(const WindowDesc &desc);
	~Window();

	void *GetNativeWindow() const;

	bool ProcessEvents();

	uint32_t GetWidth() const
	{
		return width_;
	}
	
	uint32_t GetHeight() const
	{
		return height_;
	}

	float GetAspectRatio() const
	{
		return static_cast<float>(width_) / static_cast<float>(height_);
	}

	float GetMouseWheelDelta()
	{
		float delta = static_cast<float>(mouseWheelDelta_) / 120.0f;
		mouseWheelDelta_ = 0;
		return delta;
	}

	float GetMouseDeltaX()
	{
		float delta = static_cast<float>(currentMouseX_ - lastMouseX_);
		lastMouseX_ = currentMouseX_;
		return delta;
	}
	
	float GetMouseDeltaY()
	{
		float delta = static_cast<float>(currentMouseY_ - lastMouseY_);
		lastMouseY_ = currentMouseY_;
		return delta;
	}

	using ResizeCallback = void (*)(void *userData, uint32_t width, uint32_t height);

	void SetResizeCallback(ResizeCallback callback, void *userData)
	{
		resizeCallback_ = callback;
		resizeCallbackUserData_ = userData;
	}

	bool WasKeyPressed(uint32_t virtualKeyCode) const;
	bool WasKeyToggled(uint32_t virtualKeyCode) const;

	bool IsLeftMouseButtonDown() const
	{
		return isLeftMouseButtonDown_;
	}

	bool IsRightMouseButtonDown() const
	{
		return isRightMouseButtonDown_;
	}

	static long long __stdcall WindowProc(HWND__ *hwnd, uint32_t msg, uintptr_t wParam, intptr_t lParam);

private:
	void *windowHandle_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	int mouseWheelDelta_ = 0;
	int lastMouseX_ = 0;
	int lastMouseY_ = 0;
	int currentMouseX_ = 0;
	int currentMouseY_ = 0;
	bool firstMouseMove_ = true;
	bool isLeftMouseButtonDown_ = false;
	bool isRightMouseButtonDown_ = false;
	bool keyWasToggled_[256] = {};
	ResizeCallback resizeCallback_ = nullptr;
	void *resizeCallbackUserData_ = nullptr;
};

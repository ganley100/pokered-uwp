// UWP (CoreWindow) host for one Game Boy cartridge. Runs on Xbox in Developer
// Mode and on Windows desktop. The cartridge is Game.gb in the app package.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <unknwn.h>
#include <windows.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <xaudio2.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "GameConfig.h"
#include "GbCore.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Core;

namespace
{
	// One Game Boy frame is 70224 cycles of a 4.194304 MHz clock (~59.73 Hz).
	constexpr double kFramePeriod = 70224.0 / 4194304.0;
	constexpr int kFastForwardSpeed = 4;
	constexpr int kMaxFramesPerTick = 8;
	constexpr int kSaveCheckInterval = 60; // frames
	constexpr float kStickThreshold = 0.5f;

	// ------------------------------------------------------------------
	// File helpers
	// ------------------------------------------------------------------

	bool ReadWholeFile(const std::wstring &path, std::vector<uint8_t> &out)
	{
		FILE *f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
			return false;

		bool ok = false;
		if (fseek(f, 0, SEEK_END) == 0)
		{
			const long size = ftell(f);
			if (size >= 0 && fseek(f, 0, SEEK_SET) == 0)
			{
				out.resize(static_cast<size_t>(size));
				ok = size == 0 || fread(out.data(), 1, out.size(), f) == out.size();
			}
		}
		fclose(f);
		return ok;
	}

	bool WriteFileAtomically(const std::wstring &path, const uint8_t *data, size_t size)
	{
		const std::wstring tmp = path + L".tmp";
		FILE *f = nullptr;
		if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || f == nullptr)
			return false;

		const bool written = fwrite(data, 1, size, f) == size;
		const bool closed = fclose(f) == 0;
		if (!written || !closed)
		{
			_wremove(tmp.c_str());
			return false;
		}

		_wremove(path.c_str());
		return _wrename(tmp.c_str(), path.c_str()) == 0;
	}

	// ------------------------------------------------------------------
	// Audio: XAudio2 source voice fed one buffer per emulated frame.
	// ------------------------------------------------------------------

	class AudioOutput
	{
	public:
		~AudioOutput()
		{
			if (m_source)
				m_source->DestroyVoice();
			if (m_master)
				m_master->DestroyVoice();
		}

		void Initialize(unsigned sampleRate, unsigned framesPerBuffer)
		{
			if (FAILED(XAudio2Create(m_xaudio.put(), 0, XAUDIO2_DEFAULT_PROCESSOR)))
				return;
			if (FAILED(m_xaudio->CreateMasteringVoice(&m_master)))
				return;

			WAVEFORMATEX format = {};
			format.wFormatTag = WAVE_FORMAT_PCM;
			format.nChannels = 2;
			format.nSamplesPerSec = sampleRate;
			format.wBitsPerSample = 16;
			format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
			format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

			if (FAILED(m_xaudio->CreateSourceVoice(&m_source, &format)))
				return;

			for (auto &buffer : m_buffers)
				buffer.resize(framesPerBuffer * 2);
			m_source->Start();
		}

		void Submit(const int16_t *samples)
		{
			if (!m_source)
				return;

			// Each ring slot may only be reused once XAudio2 is finished with
			// it. Keeping fewer than kBuffers - 1 queued guarantees that, and
			// also drops audio while fast-forwarding instead of piling it up.
			XAUDIO2_VOICE_STATE state = {};
			m_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			if (state.BuffersQueued >= kBuffers - 2)
				return;

			auto &buffer = m_buffers[m_next];
			m_next = (m_next + 1) % kBuffers;
			std::copy(samples, samples + buffer.size(), buffer.begin());

			XAUDIO2_BUFFER xb = {};
			xb.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(int16_t));
			xb.pAudioData = reinterpret_cast<const BYTE *>(buffer.data());
			m_source->SubmitSourceBuffer(&xb);
		}

		void Suspend()
		{
			if (m_xaudio)
				m_xaudio->StopEngine();
		}

		void Resume()
		{
			if (m_xaudio)
				m_xaudio->StartEngine();
		}

	private:
		static constexpr int kBuffers = 8;
		com_ptr<IXAudio2> m_xaudio;
		IXAudio2MasteringVoice *m_master = nullptr;
		IXAudio2SourceVoice *m_source = nullptr;
		std::vector<int16_t> m_buffers[kBuffers];
		int m_next = 0;
	};

	// ------------------------------------------------------------------
	// Video: Direct2D drawing onto a D3D11 swap chain for the CoreWindow.
	// ------------------------------------------------------------------

	class Renderer
	{
	public:
		void Initialize(CoreWindow const &window)
		{
			m_window = window;
			check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				__uuidof(ID2D1Factory1), nullptr, m_d2dFactory.put_void()));
			check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
				__uuidof(IDWriteFactory), reinterpret_cast<::IUnknown **>(m_dwriteFactory.put())));
			CreateDeviceResources();
			CreateWindowResources();
		}

		void OnWindowChanged()
		{
			if (m_swapChain)
				CreateWindowResources();
		}

		void Trim()
		{
			if (m_d3dDevice)
				m_d3dDevice.as<IDXGIDevice3>()->Trim();
		}

		// `pixels` is GBCORE_WIDTH x GBCORE_HEIGHT BGRA, or null for none.
		void Present(const uint32_t *pixels, bool integerScale,
			const std::wstring &overlay, const std::wstring &message)
		{
			if (!m_targetBitmap)
				return;

			m_d2dContext->BeginDraw();
			m_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f));

			const float w = static_cast<float>(m_width);
			const float h = static_cast<float>(m_height);

			if (pixels)
			{
				m_gameBitmap->CopyFromMemory(nullptr, pixels, GBCORE_WIDTH * 4);

				float scale = std::min(w / GBCORE_WIDTH, h / GBCORE_HEIGHT);
				if (integerScale && scale >= 1.0f)
					scale = std::floor(scale);
				const float dw = GBCORE_WIDTH * scale;
				const float dh = GBCORE_HEIGHT * scale;
				const float x = std::floor((w - dw) / 2);
				const float y = std::floor((h - dh) / 2);
				const D2D1_RECT_F dest = D2D1::RectF(x, y, x + dw, y + dh);
				m_d2dContext->DrawBitmap(m_gameBitmap.get(), &dest, 1.0f,
					D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
			}

			if (!message.empty())
				DrawTextBox(message, D2D1::RectF(w * 0.1f, h * 0.3f, w * 0.9f, h * 0.7f), DWRITE_TEXT_ALIGNMENT_CENTER);

			if (!overlay.empty())
				DrawTextBox(overlay, D2D1::RectF(w * 0.05f, h * 0.88f, w * 0.95f, h * 0.97f), DWRITE_TEXT_ALIGNMENT_CENTER);

			HRESULT hr = m_d2dContext->EndDraw();
			if (hr == D2DERR_RECREATE_TARGET)
			{
				HandleDeviceLost();
				return;
			}

			hr = m_swapChain->Present(1, 0);
			if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
				HandleDeviceLost();
		}

	private:
		void CreateDeviceResources()
		{
			const D3D_FEATURE_LEVEL levels[] = {
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
				D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
				D3D_FEATURE_LEVEL_9_3,
			};
			const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

			com_ptr<ID3D11Device> device;
			HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
				levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
			if (FAILED(hr))
			{
				check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
					levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr));
			}
			m_d3dDevice = device;

			auto dxgiDevice = m_d3dDevice.as<IDXGIDevice1>();
			check_hresult(m_d2dFactory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));
			check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));
			m_d2dContext->SetDpi(96.0f, 96.0f);
			m_d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

			const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
			check_hresult(m_d2dContext->CreateBitmap(D2D1::SizeU(GBCORE_WIDTH, GBCORE_HEIGHT),
				nullptr, 0, &props, m_gameBitmap.put()));

			check_hresult(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_textBrush.put()));
			check_hresult(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f), m_backBrush.put()));
		}

		void CreateWindowResources()
		{
			const auto display = DisplayInformation::GetForCurrentView();
			const float dpi = display.LogicalDpi();
			const auto bounds = m_window.Bounds();
			m_width = std::max(1u, static_cast<UINT>(std::lround(bounds.Width * dpi / 96.0f)));
			m_height = std::max(1u, static_cast<UINT>(std::lround(bounds.Height * dpi / 96.0f)));

			m_d2dContext->SetTarget(nullptr);
			m_targetBitmap = nullptr;

			if (m_swapChain)
			{
				HRESULT hr = m_swapChain->ResizeBuffers(2, m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
				if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
				{
					HandleDeviceLost();
					return;
				}
				check_hresult(hr);
			}
			else
			{
				DXGI_SWAP_CHAIN_DESC1 desc = {};
				desc.Width = m_width;
				desc.Height = m_height;
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				desc.BufferCount = 2;
				desc.Scaling = DXGI_SCALING_STRETCH;
				desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
				desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

				auto dxgiDevice = m_d3dDevice.as<IDXGIDevice1>();
				com_ptr<IDXGIAdapter> adapter;
				check_hresult(dxgiDevice->GetAdapter(adapter.put()));
				com_ptr<IDXGIFactory2> factory;
				check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
				check_hresult(factory->CreateSwapChainForCoreWindow(m_d3dDevice.get(),
					winrt::get_unknown(m_window), &desc, nullptr, m_swapChain.put()));
				dxgiDevice->SetMaximumFrameLatency(1);
			}

			com_ptr<IDXGISurface> surface;
			check_hresult(m_swapChain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));
			const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
				D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
			check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(surface.get(), &props, m_targetBitmap.put()));
			m_d2dContext->SetTarget(m_targetBitmap.get());

			// Text scales with the screen so it stays readable from the couch.
			m_textFormat = nullptr;
			check_hresult(m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
				DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
				std::max(14.0f, m_height / 30.0f), L"en-us", m_textFormat.put()));
			m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
		}

		void HandleDeviceLost()
		{
			m_d2dContext = nullptr;
			m_targetBitmap = nullptr;
			m_gameBitmap = nullptr;
			m_textBrush = nullptr;
			m_backBrush = nullptr;
			m_d2dDevice = nullptr;
			m_swapChain = nullptr;
			m_d3dDevice = nullptr;
			CreateDeviceResources();
			CreateWindowResources();
		}

		void DrawTextBox(const std::wstring &text, D2D1_RECT_F rect, DWRITE_TEXT_ALIGNMENT align)
		{
			m_textFormat->SetTextAlignment(align);
			com_ptr<IDWriteTextLayout> layout;
			if (FAILED(m_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
				m_textFormat.get(), rect.right - rect.left, rect.bottom - rect.top, layout.put())))
				return;

			DWRITE_TEXT_METRICS metrics = {};
			layout->GetMetrics(&metrics);
			const float pad = m_height / 60.0f;
			const D2D1_RECT_F back = D2D1::RectF(
				rect.left + metrics.left - pad, rect.top + metrics.top - pad,
				rect.left + metrics.left + metrics.width + pad, rect.top + metrics.top + metrics.height + pad);
			m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(back, pad, pad), m_backBrush.get());
			m_d2dContext->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.get(), m_textBrush.get());
		}

		CoreWindow m_window{ nullptr };
		UINT m_width = 1;
		UINT m_height = 1;
		com_ptr<ID2D1Factory1> m_d2dFactory;
		com_ptr<IDWriteFactory> m_dwriteFactory;
		com_ptr<ID3D11Device> m_d3dDevice;
		com_ptr<ID2D1Device> m_d2dDevice;
		com_ptr<ID2D1DeviceContext> m_d2dContext;
		com_ptr<IDXGISwapChain1> m_swapChain;
		com_ptr<ID2D1Bitmap1> m_targetBitmap;
		com_ptr<ID2D1Bitmap1> m_gameBitmap;
		com_ptr<ID2D1SolidColorBrush> m_textBrush;
		com_ptr<ID2D1SolidColorBrush> m_backBrush;
		com_ptr<IDWriteTextFormat> m_textFormat;
	};

	// ------------------------------------------------------------------
	// Input: every connected gamepad plus the keyboard.
	// ------------------------------------------------------------------

	struct InputState
	{
		uint8_t buttons = 0;     // GBCORE_BTN_*
		bool fastForward = false;
		bool nextPalette = false;
		bool toggleScaling = false;
	};

	bool Has(GamepadButtons held, GamepadButtons flag)
	{
		return (static_cast<uint32_t>(held) & static_cast<uint32_t>(flag)) != 0;
	}
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
	IFrameworkView CreateView()
	{
		return *this;
	}

	void Initialize(CoreApplicationView const &view)
	{
		view.Activated({ this, &App::OnActivated });
		CoreApplication::Suspending({ this, &App::OnSuspending });
		CoreApplication::Resuming({ this, &App::OnResuming });

		// Gamepad::Gamepads() only reports controllers reliably once
		// something is listening for them to connect.
		Gamepad::GamepadAdded([](auto &&, auto &&) {});
	}

	void Load(hstring const &)
	{
	}

	void Uninitialize()
	{
	}

	void SetWindow(CoreWindow const &window)
	{
		m_window = window;
		window.KeyDown({ this, &App::OnKeyDown });
		window.KeyUp({ this, &App::OnKeyUp });
		window.SizeChanged([this](auto &&, auto &&) { m_renderer.OnWindowChanged(); });
		window.VisibilityChanged([this](auto &&, VisibilityChangedEventArgs const &args) { m_visible = args.Visible(); });
		window.Closed([this](auto &&, auto &&) { m_closed = true; });
		DisplayInformation::GetForCurrentView().DpiChanged([this](auto &&, auto &&) { m_renderer.OnWindowChanged(); });

		// On Xbox the B button raises a system "back" request, which would
		// otherwise send the app to the background. B belongs to the game.
		SystemNavigationManager::GetForCurrentView().BackRequested(
			[](auto &&, BackRequestedEventArgs const &args) { args.Handled(true); });
	}

	void Run()
	{
		LoadSettings();
		m_renderer.Initialize(m_window);
		m_message = LoadGame();
		m_audio.Initialize(gbcore_audio_sample_rate(), gbcore_audio_frames_per_video_frame());
		ShowOverlay(std::wstring(GameConfig::kTitle) + L"   •   Y: colours   •   RT: fast-forward");

		LARGE_INTEGER frequency, last, now;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&last);
		double accumulator = 0.0;

		while (!m_closed)
		{
			if (!m_visible)
			{
				m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
				QueryPerformanceCounter(&last);
				accumulator = 0.0;
				continue;
			}

			m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

			QueryPerformanceCounter(&now);
			double elapsed = static_cast<double>(now.QuadPart - last.QuadPart) / frequency.QuadPart;
			last = now;
			elapsed = std::min(elapsed, 0.25);
			if (m_overlayTime > 0.0)
				m_overlayTime -= elapsed;

			const InputState input = PollInput();
			HandleMenuInput(input);

			if (m_running)
			{
				accumulator += elapsed * (input.fastForward ? kFastForwardSpeed : 1);
				int frames = 0;
				{
					std::lock_guard<std::mutex> lock(m_coreLock);
					gbcore_set_buttons(input.buttons);
					while (accumulator >= kFramePeriod && frames < kMaxFramesPerTick)
					{
						gbcore_run_frame();
						m_audio.Submit(gbcore_audio_buffer());
						accumulator -= kFramePeriod;
						++frames;

						if (++m_framesSinceSaveCheck >= kSaveCheckInterval)
						{
							m_framesSinceSaveCheck = 0;
							SaveIfDirty();
						}
					}
				}
				if (frames == kMaxFramesPerTick)
					accumulator = 0.0; // Too slow to keep up: drop the backlog.

				if (gbcore_crashed() && m_message.empty())
				{
					m_message = L"The game stopped responding.\nHold Start + Select + A + B to restart it.";
				}
			}

			BuildPixels();
			m_renderer.Present(m_running ? m_pixels.data() : nullptr, m_integerScale,
				m_overlayTime > 0.0 ? m_overlay : std::wstring(), m_message);
		}

		std::lock_guard<std::mutex> lock(m_coreLock);
		SaveIfDirty();
		gbcore_shutdown();
	}

private:
	std::wstring LoadGame()
	{
		const std::wstring romPath =
			std::wstring(Package::Current().InstalledLocation().Path()) + L"\\Game.gb";
		std::vector<uint8_t> rom;
		if (!ReadWholeFile(romPath, rom))
			return L"Game.gb is missing from this app package.\n"
				L"Build the ROM with make before packaging the app (see uwp/README.md).";

		switch (gbcore_init(rom.data(), rom.size()))
		{
		case GBCORE_OK:
			break;
		case GBCORE_ERR_BAD_CHECKSUM:
			return L"Game.gb has a bad header checksum.";
		case GBCORE_ERR_UNSUPPORTED_CART:
			return L"Game.gb uses an unsupported cartridge type.";
		default:
			return L"Game.gb could not be loaded.";
		}

		m_savePath = std::wstring(ApplicationData::Current().LocalFolder().Path()) + L"\\" + GameConfig::kSaveFile;
		std::vector<uint8_t> save;
		if (gbcore_save_size() > 0)
		{
			// A leftover .tmp means the app stopped between writing a save
			// and renaming it into place, so it is the newest one.
			if ((ReadWholeFile(m_savePath + L".tmp", save) && save.size() == gbcore_save_size()) ||
				(ReadWholeFile(m_savePath, save) && save.size() == gbcore_save_size()))
			{
				std::copy(save.begin(), save.end(), gbcore_save_data());
			}
		}
		gbcore_clear_save_dirty();

		m_running = true;
		return {};
	}

	// Call with m_coreLock held.
	void SaveIfDirty()
	{
		if (!m_running || !gbcore_save_dirty() || gbcore_save_size() == 0)
			return;
		if (WriteFileAtomically(m_savePath, gbcore_save_data(), gbcore_save_size()))
			gbcore_clear_save_dirty();
	}

	void BuildPixels()
	{
		if (!m_running)
			return;

		const auto &palette = GameConfig::kPalettes[m_paletteIndex];
		const uint32_t *layers[4] = { palette.obj0, palette.obj1, palette.bg, palette.bg };
		const uint8_t *fb = gbcore_framebuffer();
		m_pixels.resize(GBCORE_WIDTH * GBCORE_HEIGHT);
		for (size_t i = 0; i < m_pixels.size(); ++i)
		{
			const uint8_t p = fb[i];
			m_pixels[i] = 0xFF000000u | layers[(p >> 4) & 3][p & 3];
		}
	}

	InputState PollInput()
	{
		InputState s;
		uint32_t pad = 0;
		float stickX = 0.0f, stickY = 0.0f, rightTrigger = 0.0f;

		for (auto const &gamepad : Gamepad::Gamepads())
		{
			const GamepadReading r = gamepad.GetCurrentReading();
			pad |= static_cast<uint32_t>(r.Buttons);
			if (std::fabs(r.LeftThumbstickX) > std::fabs(stickX))
				stickX = static_cast<float>(r.LeftThumbstickX);
			if (std::fabs(r.LeftThumbstickY) > std::fabs(stickY))
				stickY = static_cast<float>(r.LeftThumbstickY);
			rightTrigger = std::max(rightTrigger, static_cast<float>(r.RightTrigger));
		}
		const auto held = static_cast<GamepadButtons>(pad);

		auto key = [this](VirtualKey k) { return m_keys[static_cast<uint8_t>(k)]; };
		auto ch = [&](char c) { return key(static_cast<VirtualKey>(c)); };

		if (Has(held, GamepadButtons::A) || ch('X') || ch('K'))
			s.buttons |= GBCORE_BTN_A;
		if (Has(held, GamepadButtons::B) || Has(held, GamepadButtons::X) || ch('Z') || ch('J'))
			s.buttons |= GBCORE_BTN_B;
		if (Has(held, GamepadButtons::View) || key(VirtualKey::Back) || key(VirtualKey::Shift))
			s.buttons |= GBCORE_BTN_SELECT;
		if (Has(held, GamepadButtons::Menu) || key(VirtualKey::Enter))
			s.buttons |= GBCORE_BTN_START;
		if (Has(held, GamepadButtons::DPadUp) || stickY > kStickThreshold || key(VirtualKey::Up) || ch('W'))
			s.buttons |= GBCORE_BTN_UP;
		if (Has(held, GamepadButtons::DPadDown) || stickY < -kStickThreshold || key(VirtualKey::Down) || ch('S'))
			s.buttons |= GBCORE_BTN_DOWN;
		if (Has(held, GamepadButtons::DPadLeft) || stickX < -kStickThreshold || key(VirtualKey::Left) || ch('A'))
			s.buttons |= GBCORE_BTN_LEFT;
		if (Has(held, GamepadButtons::DPadRight) || stickX > kStickThreshold || key(VirtualKey::Right) || ch('D'))
			s.buttons |= GBCORE_BTN_RIGHT;

		// The D-pad cannot press opposite directions at once.
		if ((s.buttons & (GBCORE_BTN_UP | GBCORE_BTN_DOWN)) == (GBCORE_BTN_UP | GBCORE_BTN_DOWN))
			s.buttons &= ~(GBCORE_BTN_UP | GBCORE_BTN_DOWN);
		if ((s.buttons & (GBCORE_BTN_LEFT | GBCORE_BTN_RIGHT)) == (GBCORE_BTN_LEFT | GBCORE_BTN_RIGHT))
			s.buttons &= ~(GBCORE_BTN_LEFT | GBCORE_BTN_RIGHT);

		s.fastForward = rightTrigger > 0.5f || key(VirtualKey::Tab);

		const bool palette = Has(held, GamepadButtons::Y) || ch('P');
		const bool scaling = Has(held, GamepadButtons::LeftThumbstick) || ch('F');
		s.nextPalette = palette && !m_paletteHeld;
		s.toggleScaling = scaling && !m_scalingHeld;
		m_paletteHeld = palette;
		m_scalingHeld = scaling;
		return s;
	}

	void HandleMenuInput(InputState const &input)
	{
		if (input.nextPalette)
		{
			m_paletteIndex = (m_paletteIndex + 1) % GameConfig::kPaletteCount;
			ShowOverlay(std::wstring(L"Colours: ") + GameConfig::kPalettes[m_paletteIndex].name);
			SaveSettings();
		}
		if (input.toggleScaling)
		{
			m_integerScale = !m_integerScale;
			ShowOverlay(m_integerScale ? L"Scaling: pixel-perfect" : L"Scaling: fill screen");
			SaveSettings();
		}

		constexpr uint8_t kResetCombo = GBCORE_BTN_A | GBCORE_BTN_B | GBCORE_BTN_START | GBCORE_BTN_SELECT;
		if (m_running && gbcore_crashed() && (input.buttons & kResetCombo) == kResetCombo)
		{
			std::lock_guard<std::mutex> lock(m_coreLock);
			gbcore_reset();
			m_message.clear();
		}
	}

	void ShowOverlay(std::wstring text)
	{
		m_overlay = std::move(text);
		m_overlayTime = 3.0;
	}

	void LoadSettings()
	{
		const auto values = ApplicationData::Current().LocalSettings().Values();
		const int palette = unbox_value_or<int32_t>(values.TryLookup(L"palette"), 0);
		m_paletteIndex = (palette >= 0 && palette < GameConfig::kPaletteCount) ? palette : 0;
		m_integerScale = unbox_value_or<bool>(values.TryLookup(L"integerScale"), true);
	}

	void SaveSettings()
	{
		const auto values = ApplicationData::Current().LocalSettings().Values();
		values.Insert(L"palette", box_value(static_cast<int32_t>(m_paletteIndex)));
		values.Insert(L"integerScale", box_value(m_integerScale));
	}

	void OnActivated(CoreApplicationView const &, IActivatedEventArgs const &)
	{
		CoreWindow::GetForCurrentThread().Activate();
	}

	void OnSuspending(winrt::Windows::Foundation::IInspectable const &, SuspendingEventArgs const &)
	{
		{
			std::lock_guard<std::mutex> lock(m_coreLock);
			SaveIfDirty();
		}
		m_audio.Suspend();
		m_renderer.Trim();
	}

	void OnResuming(winrt::Windows::Foundation::IInspectable const &, winrt::Windows::Foundation::IInspectable const &)
	{
		m_audio.Resume();
	}

	void OnKeyDown(CoreWindow const &, KeyEventArgs const &args)
	{
		m_keys[static_cast<uint8_t>(args.VirtualKey())] = true;
	}

	void OnKeyUp(CoreWindow const &, KeyEventArgs const &args)
	{
		m_keys[static_cast<uint8_t>(args.VirtualKey())] = false;
	}

	CoreWindow m_window{ nullptr };
	Renderer m_renderer;
	AudioOutput m_audio;
	std::mutex m_coreLock;

	bool m_closed = false;
	bool m_visible = true;
	bool m_running = false;
	std::wstring m_message;
	std::wstring m_overlay;
	double m_overlayTime = 0.0;
	std::wstring m_savePath;
	int m_framesSinceSaveCheck = 0;

	std::vector<uint32_t> m_pixels;
	int m_paletteIndex = 0;
	bool m_integerScale = true;

	bool m_keys[256] = {};
	bool m_paletteHeld = false;
	bool m_scalingHeld = false;
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	CoreApplication::Run(make<App>());
	return 0;
}

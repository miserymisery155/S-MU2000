// license:BSD-3-Clause

#include "audio_in.h"
#include "resampler.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {

namespace {

// PKEY_Device_FriendlyName（audio_out.cpp と同じ）
const PROPERTYKEY kFriendlyName = {
	{ 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

std::string endpoint_name(IMMDevice *d)
{
	std::string out;
	IPropertyStore *ps = nullptr;
	if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)))
		return out;
	PROPVARIANT v;
	PropVariantInit(&v);
	if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.pwszVal) {
		char buf[256] = {};
		WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
		out = buf;
	}
	PropVariantClear(&v);
	ps->Release();
	return out;
}

bool format_is_float(const WAVEFORMATEX *f)
{
	if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		return reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f)->SubFormat.Data1 == 3;
	return false;
}

} // namespace


std::vector<std::string> audio_in::list()
{
	std::vector<std::string> out;
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	IMMDeviceEnumerator *en = nullptr;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                               __uuidof(IMMDeviceEnumerator), (void **)&en))) {
		IMMDeviceCollection *all = nullptr;
		if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all))) {
			UINT n = 0;
			all->GetCount(&n);
			for (UINT i = 0; i < n; i++) {
				IMMDevice *d = nullptr;
				if (SUCCEEDED(all->Item(i, &d))) {
					out.push_back(endpoint_name(d));
					d->Release();
				}
			}
			all->Release();
		}
		en->Release();
	}
	if (com)
		CoUninitialize();
	return out;
}

bool audio_in::start(const std::string &device, std::string &err)
{
	if (m_thread.joinable())
		return true;
	m_want = device;
	m_quit = false;
	m_start_state = 0;
	m_w = 0;
	m_r = 0;
	m_thread = std::thread([this] { run(); });
	while (m_start_state.load() == 0)
		Sleep(1);
	if (m_start_state.load() != 1) {
		err = m_err;
		m_thread.join();
		return false;
	}
	return true;
}

void audio_in::stop()
{
	if (!m_thread.joinable())
		return;
	m_quit = true;
	m_thread.join();
	m_running = false;
}

std::string audio_in::format_line() const
{
	char buf[160];
	std::snprintf(buf, sizeof buf, "共有 / %u Hz %u ch %s%u → 44100 Hz", m_dev_rate, m_dev_channels,
	              m_dev_float ? "float" : "int", m_dev_bits);
	return buf;
}

// 44100Hz 16bit 2ch を輪に積む。溢れる分は捨てる（読み手が止まっている）
void audio_in::push(const s16 *frames, u32 n)
{
	u32 wr = m_w.load(std::memory_order_relaxed);
	for (u32 i = 0; i < n; i++) {
		const u32 rd = m_r.load(std::memory_order_acquire);
		if (((wr + 1) & MASK) == rd)
			break;
		m_ring[wr * 2] = frames[i * 2];
		m_ring[wr * 2 + 1] = frames[i * 2 + 1];
		wr = (wr + 1) & MASK;
		m_w.store(wr, std::memory_order_release);
	}
}

void audio_in::run()
{
	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioClient *client = nullptr;
	IAudioCaptureClient *cap = nullptr;
	WAVEFORMATEX *mix = nullptr;
	HANDLE event = nullptr;
	HANDLE task = nullptr;
	resampler rs;
	std::vector<s16> staging;
	std::vector<float> conv;
	std::vector<s16> out16;
	HRESULT hr;

	auto fail = [&](const char *what, HRESULT h) {
		char buf[128];
		std::snprintf(buf, sizeof buf, "%s に失敗（0x%08lx）", what, (unsigned long)h);
		m_err = buf;
		m_start_state = 2;
	};

	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                      __uuidof(IMMDeviceEnumerator), (void **)&en);
	if (FAILED(hr)) { fail("デバイス一覧", hr); goto done; }

	if (!m_want.empty()) {
		IMMDeviceCollection *all = nullptr;
		if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all))) {
			UINT n = 0;
			all->GetCount(&n);
			for (UINT i = 0; i < n && !dev; i++) {
				IMMDevice *d = nullptr;
				if (SUCCEEDED(all->Item(i, &d))) {
					if (endpoint_name(d) == m_want)
						dev = d;
					else
						d->Release();
				}
			}
			all->Release();
		}
		if (!dev) { m_err = "録音デバイスが見つからない: " + m_want; m_start_state = 2; goto done; }
	} else {
		hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
		if (FAILED(hr)) { fail("既定の録音デバイス", hr); goto done; }
	}
	m_dev_name = endpoint_name(dev);

	hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
	if (FAILED(hr)) { fail("録音デバイスを開く", hr); goto done; }
	if (FAILED(client->GetMixFormat(&mix)) || !mix) { fail("形式の取得", E_FAIL); goto done; }
	m_dev_rate = mix->nSamplesPerSec;
	m_dev_channels = mix->nChannels;
	m_dev_bits = mix->wBitsPerSample;
	m_dev_float = format_is_float(mix);
	if (!(m_dev_float && m_dev_bits == 32) && !(!m_dev_float && (m_dev_bits == 16 || m_dev_bits == 24 || m_dev_bits == 32))) {
		fail("扱えない形式", E_FAIL);
		goto done;
	}

	event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
	                        1000000 /* 100ms */, 0, mix, nullptr);
	if (FAILED(hr)) { fail("録音の準備", hr); goto done; }
	client->SetEventHandle(event);
	hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap);
	if (FAILED(hr)) { fail("録音の口", hr); goto done; }

	rs.configure(double(m_dev_rate), 44100.0);
	{
		DWORD idx = 0;
		task = AvSetMmThreadCharacteristicsA("Pro Audio", &idx);
	}
	hr = client->Start();
	if (FAILED(hr)) { fail("録音の開始", hr); goto done; }

	m_running = true;
	m_start_state = 1;

	while (!m_quit.load()) {
		WaitForSingleObject(event, 200);
		UINT32 packet = 0;
		while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
				break;
			// 2ch の 16bit に直す。1ch なら両方に、3ch 以上は頭の 2 つ
			staging.resize(size_t(frames) * 2);
			const u32 ch = m_dev_channels, bytes = m_dev_bits / 8;
			for (UINT32 i = 0; i < frames; i++) {
				for (u32 c = 0; c < 2; c++) {
					const u32 src = std::min(c, ch - 1);
					s32 v = 0;
					if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
						const BYTE *p = data + (size_t(i) * ch + src) * bytes;
						if (m_dev_float) {
							float f;
							std::memcpy(&f, p, 4);
							v = s32(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
						} else if (bytes == 2) {
							v = s16(p[0] | (p[1] << 8));
						} else if (bytes == 3) {
							v = s16(p[1] | (p[2] << 8));          // 上の 16bit
						} else {
							v = s16(p[2] | (p[3] << 8));
						}
					}
					staging[size_t(i) * 2 + c] = s16(v);
				}
			}
			cap->ReleaseBuffer(frames);

			if (rs.direct()) {
				push(staging.data(), frames);
				continue;
			}
			// 変換器の輪は 4096 フレーム。1 回に入れる量をそれより小さく刻む
			for (UINT32 at = 0; at < frames;) {
				const UINT32 k = std::min<UINT32>(1024, frames - at);
				rs.push(staging.data() + size_t(at) * 2, int(k));
				at += k;
				const int n = rs.output_available();
				if (n <= 0)
					continue;
				conv.resize(size_t(n) * 2);
				out16.resize(size_t(n) * 2);
				rs.pull(conv.data(), n);
				for (size_t j = 0; j < out16.size(); j++)
					out16[j] = s16(std::lround(std::clamp(conv[j], -1.0f, 1.0f) * 32767.0f));
				push(out16.data(), u32(n));
			}
		}
	}
	client->Stop();

done:
	m_running = false;
	if (m_start_state.load() == 0)
		m_start_state = 2;
	if (task) AvRevertMmThreadCharacteristics(task);
	if (cap) cap->Release();
	if (mix) CoTaskMemFree(mix);
	if (client) client->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	if (event) CloseHandle(event);
	if (com) CoUninitialize();
}

} // namespace ui

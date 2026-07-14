#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <ksmedia.h>

#pragma comment(lib, "ole32.lib")

int main(int argc, char **argv) {
  // argv[1] = duration seconds (default 4), argv[2] = amplitude 0..1 (default 0.3)
  double durationSec = argc > 1 ? atof(argv[1]) : 4.0;
  double amplitude = argc > 2 ? atof(argv[2]) : 0.3;
  if (durationSec <= 0) durationSec = 4.0;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
  if (FAILED(hr)) { printf("CoCreateInstance failed hr=0x%08x\n", hr); return 1; }

  IMMDeviceCollection *devices = nullptr;
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
  if (FAILED(hr)) { printf("EnumAudioEndpoints failed hr=0x%08x\n", hr); return 1; }

  UINT count = 0;
  devices->GetCount(&count);
  IMMDevice *target = nullptr;
  for (UINT i = 0; i < count; ++i) {
    IMMDevice *dev = nullptr;
    devices->Item(i, &dev);
    IPropertyStore *props = nullptr;
    dev->OpenPropertyStore(STGM_READ, &props);
    PROPVARIANT pv; PropVariantInit(&pv);
    props->GetValue(PKEY_Device_FriendlyName, &pv);
    std::wstring name = pv.pwszVal ? pv.pwszVal : L"";
    wprintf(L"Render endpoint %u: %s\n", i, name.c_str());
    if (name.find(L"DualSense") != std::wstring::npos) {
      target = dev;
      PropVariantClear(&pv);
      props->Release();
      break;
    }
    PropVariantClear(&pv);
    props->Release();
    dev->Release();
  }

  if (!target) { printf("No DualSense render endpoint found.\n"); return 2; }

  IAudioClient *client = nullptr;
  hr = target->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
  if (FAILED(hr)) { printf("Activate failed hr=0x%08x\n", hr); return 1; }

  WAVEFORMATEX *mixFormat = nullptr;
  client->GetMixFormat(&mixFormat);
  printf("Mix format: %u Hz, %u ch, %u bits\n", mixFormat->nSamplesPerSec,
         mixFormat->nChannels, mixFormat->wBitsPerSample);

  REFERENCE_TIME bufferDuration = 10000000; // 1 second buffer
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat, nullptr);
  if (FAILED(hr)) { printf("Initialize failed hr=0x%08x\n", hr); return 1; }

  UINT32 bufferFrameCount = 0;
  client->GetBufferSize(&bufferFrameCount);

  IAudioRenderClient *renderClient = nullptr;
  hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
  if (FAILED(hr)) { printf("GetService failed hr=0x%08x\n", hr); return 1; }

  // Prime the full buffer with a 440Hz sine wave (or silence for float format safety).
  BYTE *data = nullptr;
  hr = renderClient->GetBuffer(bufferFrameCount, &data);
  if (FAILED(hr)) { printf("GetBuffer failed hr=0x%08x\n", hr); return 1; }

  double freq = 440.0;
  double sampleRate = mixFormat->nSamplesPerSec;
  UINT32 channels = mixFormat->nChannels;
  bool isFloat = false;
  if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    isFloat = true;
  } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             mixFormat->cbSize >= 22) {
    WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE*)mixFormat;
    isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  }
  printf("isFloat=%d wBitsPerSample=%u\n", isFloat, mixFormat->wBitsPerSample);
  for (UINT32 i = 0; i < bufferFrameCount; ++i) {
    double t = (double)i / sampleRate;
    double sample = sin(2.0 * 3.14159265 * freq * t) * amplitude;
    for (UINT32 c = 0; c < channels; ++c) {
      if (isFloat) {
        ((float*)data)[i*channels + c] = (float)sample;
      } else {
        ((short*)data)[i*channels + c] = (short)(sample * 32767.0);
      }
    }
  }
  renderClient->ReleaseBuffer(bufferFrameCount, 0);

  hr = client->Start();
  if (FAILED(hr)) { printf("Start failed hr=0x%08x\n", hr); return 1; }
  printf("Playing 440Hz tone for %.1f seconds at amplitude %.2f...\n",
         durationSec, amplitude);

  DWORD startTick = GetTickCount();
  while (GetTickCount() - startTick < (DWORD)(durationSec * 1000.0)) {
    Sleep(50);
    UINT32 padding = 0;
    client->GetCurrentPadding(&padding);
    UINT32 framesAvailable = bufferFrameCount - padding;
    if (framesAvailable == 0) continue;
    BYTE *buf = nullptr;
    if (FAILED(renderClient->GetBuffer(framesAvailable, &buf))) continue;
    static double phase = 0.0;
    for (UINT32 i = 0; i < framesAvailable; ++i) {
      double sample = sin(phase) * amplitude;
      phase += 2.0 * 3.14159265 * freq / sampleRate;
      for (UINT32 c = 0; c < channels; ++c) {
        if (isFloat) ((float*)buf)[i*channels + c] = (float)sample;
        else ((short*)buf)[i*channels + c] = (short)(sample * 32767.0);
      }
    }
    renderClient->ReleaseBuffer(framesAvailable, 0);
  }

  client->Stop();
  printf("Done.\n");
  return 0;
}

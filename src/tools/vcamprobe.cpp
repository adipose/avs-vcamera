// vcamprobe: a DirectShow client for checking a video capture device, the
// avs-vcamera driver in particular.
//
//   vcamprobe list
//       Every device in CLSID_VideoInputDeviceCategory, with the formats its
//       video and audio capture pins offer through IAMStreamConfig. JSON.
//
//   vcamprobe record <out.avi> [--seconds N] [--size WxH] [--format UYVY|RGB24]
//                    [--device <substring>] [--no-audio] [--video-preview]
//       Record the device's capture pins to an uncompressed AVI with the AVI
//       mux, the way a capture application does, then print JSON with what
//       was connected. Audio comes from the same filter's audio capture pin.
//       tests/avicheck.py reads the result.
//
// Exit code 0 on success; 2 for bad arguments; 1 for anything else.

#include <windows.h>
#include <dshow.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")

static std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { r += L'\\'; r += c; }
        else if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", c); r += b; }
        else r += c;
    }
    return r;
}

static void Fail(const wchar_t* what, HRESULT hr)
{
    fwprintf(stderr, L"vcamprobe: %s failed, hr=0x%08lx\n", what, (unsigned long)hr);
    exit(1);
}

static void FreeMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt) return;
    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) mt->pUnk->Release();
    CoTaskMemFree(mt);
}

static std::wstring SubtypeName(const GUID& g)
{
    if (g == MEDIASUBTYPE_UYVY) return L"UYVY";
    if (g == MEDIASUBTYPE_YUY2) return L"YUY2";
    if (g == MEDIASUBTYPE_RGB24) return L"RGB24";
    if (g == MEDIASUBTYPE_RGB32) return L"RGB32";
    if (g == MEDIASUBTYPE_PCM) return L"PCM";
    wchar_t b[64];
    StringFromGUID2(g, b, 64);
    return b;
}

struct Device {
    std::wstring friendlyName, displayName;
    IMoniker* moniker = nullptr;
};

static std::vector<Device> ListDevices()
{
    std::vector<Device> result;
    ICreateDevEnum* devEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) Fail(L"CoCreateInstance(SystemDeviceEnum)", hr);
    IEnumMoniker* e = nullptr;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &e, 0) == S_OK) {
        IMoniker* m = nullptr;
        while (e->Next(1, &m, nullptr) == S_OK) {
            Device d;
            d.moniker = m;
            IBindCtx* ctx = nullptr;
            CreateBindCtx(0, &ctx);
            LPOLESTR dn = nullptr;
            if (SUCCEEDED(m->GetDisplayName(ctx, nullptr, &dn))) { d.displayName = dn; CoTaskMemFree(dn); }
            ctx->Release();
            IPropertyBag* bag = nullptr;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) d.friendlyName = v.bstrVal;
                VariantClear(&v);
                bag->Release();
            }
            result.push_back(d);
        }
        e->Release();
    }
    devEnum->Release();
    return result;
}

// The IAMStreamConfig of the filter's capture pin carrying the given major type, or null.
static IAMStreamConfig* FindConfig(ICaptureGraphBuilder2* cgb, IBaseFilter* f, const GUID& major)
{
    IAMStreamConfig* sc = nullptr;
    if (FAILED(cgb->FindInterface(&PIN_CATEGORY_CAPTURE, &major, f, IID_PPV_ARGS(&sc)))) return nullptr;
    return sc;
}

static void PrintCaps(IAMStreamConfig* sc, bool video)
{
    int count = 0, size = 0;
    wprintf(L"[");
    if (sc && SUCCEEDED(sc->GetNumberOfCapabilities(&count, &size))) {
        std::vector<BYTE> caps(size);
        for (int i = 0; i < count; i++) {
            AM_MEDIA_TYPE* mt = nullptr;
            if (FAILED(sc->GetStreamCaps(i, &mt, caps.data()))) continue;
            if (i) wprintf(L", ");
            if (video && mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
                wprintf(L"{\"subtype\": \"%s\", \"width\": %ld, \"height\": %ld, \"avgTimePerFrame\": %lld}",
                        SubtypeName(mt->subtype).c_str(), vih->bmiHeader.biWidth, vih->bmiHeader.biHeight,
                        (long long)vih->AvgTimePerFrame);
            } else if (!video && mt->formattype == FORMAT_WaveFormatEx && mt->cbFormat >= sizeof(WAVEFORMATEX)) {
                auto* wf = reinterpret_cast<WAVEFORMATEX*>(mt->pbFormat);
                wprintf(L"{\"subtype\": \"%s\", \"rate\": %lu, \"bits\": %u, \"channels\": %u}",
                        SubtypeName(mt->subtype).c_str(), wf->nSamplesPerSec, wf->wBitsPerSample, wf->nChannels);
            } else {
                wprintf(L"{\"subtype\": \"%s\"}", SubtypeName(mt->subtype).c_str());
            }
            FreeMediaType(mt);
        }
    }
    wprintf(L"]");
}

static int List()
{
    auto devices = ListDevices();
    ICaptureGraphBuilder2* cgb = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cgb));
    if (FAILED(hr)) Fail(L"CoCreateInstance(CaptureGraphBuilder2)", hr);

    wprintf(L"{\"devices\": [");
    for (size_t i = 0; i < devices.size(); i++) {
        auto& d = devices[i];
        wprintf(L"%s\n  {\"friendlyName\": \"%s\", \"displayName\": \"%s\"", i ? L"," : L"",
                JsonEscape(d.friendlyName).c_str(), JsonEscape(d.displayName).c_str());
        IBaseFilter* f = nullptr;
        if (SUCCEEDED(d.moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&f)))) {
            IAMStreamConfig* v = FindConfig(cgb, f, MEDIATYPE_Video);
            IAMStreamConfig* a = FindConfig(cgb, f, MEDIATYPE_Audio);
            wprintf(L", \"video\": ");
            PrintCaps(v, true);
            wprintf(L", \"audio\": ");
            PrintCaps(a, false);
            if (v) v->Release();
            if (a) a->Release();
            f->Release();
        } else {
            wprintf(L", \"error\": \"could not create the filter\"");
        }
        wprintf(L"}");
        d.moniker->Release();
    }
    wprintf(L"\n]}\n");
    cgb->Release();
    return 0;
}

static int Record(const std::wstring& outPath, double seconds, int width, int height, const std::wstring& format,
                  const std::wstring& deviceMatch, bool audio, bool videoPreview, bool audioPreview)
{
    auto devices = ListDevices();
    Device* dev = nullptr;
    for (auto& d : devices) {
        if (!dev && (deviceMatch.empty() || d.friendlyName.find(deviceMatch) != std::wstring::npos)) dev = &d;
    }
    if (!dev) { fwprintf(stderr, L"vcamprobe: no video capture device matches '%s'\n", deviceMatch.c_str()); return 1; }

    IGraphBuilder* graph = nullptr;
    ICaptureGraphBuilder2* cgb = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (FAILED(hr)) Fail(L"CoCreateInstance(FilterGraph)", hr);
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cgb));
    if (FAILED(hr)) Fail(L"CoCreateInstance(CaptureGraphBuilder2)", hr);
    cgb->SetFiltergraph(graph);

    IBaseFilter* src = nullptr;
    hr = dev->moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&src));
    if (FAILED(hr)) Fail(L"BindToObject(device)", hr);
    hr = graph->AddFilter(src, L"Capture");
    if (FAILED(hr)) Fail(L"AddFilter(device)", hr);

    // Choose the format before anything is connected: the first capability that matches.
    IAMStreamConfig* vsc = FindConfig(cgb, src, MEDIATYPE_Video);
    if (!vsc) { fwprintf(stderr, L"vcamprobe: the device has no video capture pin with IAMStreamConfig\n"); return 1; }
    int chosenW = 0, chosenH = 0;
    std::wstring chosenFormat;
    {
        int count = 0, size = 0;
        vsc->GetNumberOfCapabilities(&count, &size);
        std::vector<BYTE> caps(size);
        bool set = false;
        for (int i = 0; i < count && !set; i++) {
            AM_MEDIA_TYPE* mt = nullptr;
            if (FAILED(vsc->GetStreamCaps(i, &mt, caps.data()))) continue;
            if (mt->formattype == FORMAT_VideoInfo) {
                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
                bool sizeOk = !width || (vih->bmiHeader.biWidth == width && labs(vih->bmiHeader.biHeight) == height);
                bool formatOk = format.empty() || _wcsicmp(SubtypeName(mt->subtype).c_str(), format.c_str()) == 0;
                if (sizeOk && formatOk) {
                    hr = vsc->SetFormat(mt);
                    if (FAILED(hr)) Fail(L"IAMStreamConfig::SetFormat", hr);
                    chosenW = vih->bmiHeader.biWidth;
                    chosenH = labs(vih->bmiHeader.biHeight);
                    chosenFormat = SubtypeName(mt->subtype);
                    set = true;
                }
            }
            FreeMediaType(mt);
        }
        if (!set) { fwprintf(stderr, L"vcamprobe: no capability matches the requested size and format\n"); return 1; }
    }
    vsc->Release();

    IBaseFilter* mux = nullptr;
    IFileSinkFilter* sink = nullptr;
    hr = cgb->SetOutputFileName(&MEDIASUBTYPE_Avi, outPath.c_str(), &mux, &sink);
    if (FAILED(hr)) Fail(L"SetOutputFileName", hr);

    // --video-preview sends the video to the default renderer instead of the file, so the AVI holds the
    // audio alone: a way to see whether the audio pin delivers when nothing downstream waits on video.
    hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, src, nullptr, videoPreview ? nullptr : mux);
    if (FAILED(hr)) Fail(L"RenderStream(video)", hr);
    bool audioConnected = false;
    if (audio) {
        hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, src, nullptr, audioPreview ? nullptr : mux);
        if (FAILED(hr)) Fail(L"RenderStream(audio)", hr);
        audioConnected = true;
    }

    IMediaControl* mc = nullptr;
    graph->QueryInterface(IID_PPV_ARGS(&mc));
    hr = mc->Run();
    if (FAILED(hr)) Fail(L"IMediaControl::Run", hr);
    Sleep((DWORD)(seconds * 1000));
    hr = mc->Stop();
    if (FAILED(hr)) Fail(L"IMediaControl::Stop", hr);

    // The filters in the graph, so a converter the graph builder inserted is visible.
    std::wstring filters;
    IEnumFilters* ef = nullptr;
    if (SUCCEEDED(graph->EnumFilters(&ef))) {
        IBaseFilter* f = nullptr;
        while (ef->Next(1, &f, nullptr) == S_OK) {
            FILTER_INFO fi = {};
            if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
                if (!filters.empty()) filters += L", ";
                filters += L"\"" + JsonEscape(fi.achName) + L"\"";
                if (fi.pGraph) fi.pGraph->Release();
            }
            f->Release();
        }
        ef->Release();
    }

    wprintf(L"{\"device\": \"%s\", \"file\": \"%s\", \"width\": %d, \"height\": %d, \"format\": \"%s\", \"seconds\": %.3f, \"audio\": %s, \"filters\": [%s]}\n",
            JsonEscape(dev->friendlyName).c_str(), JsonEscape(outPath).c_str(), chosenW, chosenH, chosenFormat.c_str(),
            seconds, audioConnected ? L"true" : L"false", filters.c_str());

    mc->Release();
    sink->Release();
    mux->Release();
    src->Release();
    cgb->Release();
    graph->Release();
    for (auto& d : devices) d.moniker->Release();
    return 0;
}

static int Usage()
{
    fwprintf(stderr,
        L"usage: vcamprobe list\n"
        L"       vcamprobe record <out.avi> [--seconds N] [--size WxH] [--format UYVY|RGB24]\n"
        L"                        [--device <substring>] [--no-audio] [--video-preview]\n");
    return 2;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return Usage();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) Fail(L"CoInitializeEx", hr);

    std::wstring cmd = argv[1];
    if (cmd == L"list") return List();
    if (cmd == L"record") {
        if (argc < 3) return Usage();
        std::wstring out = argv[2];
        double seconds = 5;
        int width = 0, height = 0;
        std::wstring format, device;
        bool audio = true, videoPreview = false, audioPreview = false;
        for (int i = 3; i < argc; i++) {
            std::wstring a = argv[i];
            bool hasValue = i + 1 < argc;
            if (a == L"--seconds" && hasValue) seconds = _wtof(argv[++i]);
            else if (a == L"--size" && hasValue) { if (swscanf_s(argv[++i], L"%dx%d", &width, &height) != 2) return Usage(); }
            else if (a == L"--format" && hasValue) format = argv[++i];
            else if (a == L"--device" && hasValue) device = argv[++i];
            else if (a == L"--no-audio") audio = false;
            else if (a == L"--video-preview") videoPreview = true;
            else if (a == L"--audio-preview") audioPreview = true;
            else return Usage();
        }
        return Record(out, seconds, width, height, format, device, audio, videoPreview, audioPreview);
    }
    return Usage();
}

// Hardware H.264 on Windows (NVIDIA, Intel, AMD) through Media Foundation's asynchronous
// encoders, fed NV12 textures straight from the GPU converter: no copy through the CPU.
#include <initguid.h> // defines the CODECAPI_* GUIDs used below

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "platform/windows/d3d.hpp"

#include <strmif.h> // ICodecAPI

#include <codecapi.h>
#include <d3d10.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace spanly::platform::win {

namespace {

void setCodec(ICodecAPI* codec, const GUID& api, UINT32 value) {
    if (!codec) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    codec->SetValue(&api, &v);
}

ComPtr<IMFActivate> findEncoder(const LUID& adapter) {
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
    ComPtr<IMFAttributes> filter;
    MFCreateAttributes(&filter, 1);
    filter->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapter), sizeof adapter);
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, filter.Get(),
             &list, &count);
    ComPtr<IMFActivate> first;
    if (count > 0) first = list[0];
    for (UINT32 i = 0; i < count; ++i)
        list[i]->Release();
    CoTaskMemFree(list);
    return first;
}

LUID adapterOf(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&desc);
    return desc.AdapterLuid;
}

class HwEncoder : public Encoder {
public:
    HwEncoder(int w, int h, int fps, int bitrate) : w_(w), h_(h), fps_(fps), bitrate_(bitrate) {}

    ~HwEncoder() override {
        {
            std::scoped_lock l(m_);
            stop_ = true;
        }
        wake_.notify_all();
        if (transform_) transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (ComPtr<IMFShutdown> s; transform_ && SUCCEEDED(transform_.As(&s))) s->Shutdown(); // ends GetEvent
        if (thread_.joinable()) thread_.join();
    }

    bool open(ID3D11Device* device) {
        auto activate = findEncoder(adapterOf(device));
        if (!activate || FAILED(activate->ActivateObject(IID_PPV_ARGS(&transform_)))) return false;
        ComPtr<IMFAttributes> attrs;
        transform_->GetAttributes(&attrs);
        if (attrs) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (ComPtr<ID3D10Multithread> mt; SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt))))
            mt->SetMultithreadProtected(TRUE); // the encoder uses the device on its own threads
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(device, token)) ||
            FAILED(
                transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get()))))
            return false;
        UINT64 size = (UINT64(w_) << 32U) | UINT64(h_), rate = (UINT64(fps_) << 32U) | 1U;
        ComPtr<IMFMediaType> o, i;
        MFCreateMediaType(&o);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        o->SetUINT32(MF_MT_AVG_BITRATE, UINT32(int(bitrate_)));
        o->SetUINT64(MF_MT_FRAME_SIZE, size);
        o->SetUINT64(MF_MT_FRAME_RATE, rate);
        o->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        o->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        MFCreateMediaType(&i);
        i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        i->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        i->SetUINT64(MF_MT_FRAME_SIZE, size);
        i->SetUINT64(MF_MT_FRAME_RATE, rate);
        i->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(transform_->SetOutputType(0, o.Get(), 0)) || FAILED(transform_->SetInputType(0, i.Get(), 0)))
            return false;
        transform_.As(&codec_);
        setCodec(codec_.Get(), CODECAPI_AVLowLatencyMode, 1);
        setCodec(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setCodec(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(int(bitrate_)));
        setCodec(codec_.Get(), CODECAPI_AVEncMPVGOPSize, UINT32(fps_ * 10));
        setCodec(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        appliedBitrate_ = bitrate_;
        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(0, &info);
        providesSamples_ =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outSize_ = std::max<DWORD>(info.cbSize, DWORD(w_ * h_));
        if (FAILED(transform_.As(&events_))) return false;
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    Codec codec() const override { return Codec::H264; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void requestKeyframe() override { forceKey_ = true; }
    void setBitrate(int bps) override { bitrate_ = bps; }

    /// Keeps only the newest picture: the encoder asks for input when it is ready for one.
    void encode(const Frame& frame) override {
        auto* f = static_cast<WinFrame*>(frame.image.get());
        if (!f->texture || frame.width != w_ || frame.height != h_) return;
        {
            std::scoped_lock l(m_);
            pending_ = frame;
        }
        wake_.notify_one();
    }

private:
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (true) {
            ComPtr<IMFMediaEvent> event;
            if (FAILED(events_->GetEvent(0, &event))) break; // shut down
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == METransformNeedInput && !feed()) break;
            if (type == METransformHaveOutput) drain();
        }
        CoUninitialize();
    }

    bool feed() {
        Frame frame;
        {
            std::unique_lock l(m_);
            wake_.wait(l, [&] { return stop_ || pending_.has_value(); });
            if (stop_) return false;
            frame = *pending_;
            pending_.reset();
            captured_.push_back(frame.captured);
            if (captured_.size() > 16) captured_.pop_front();
        }
        if (int b = bitrate_; b != appliedBitrate_) {
            setCodec(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(b));
            appliedBitrate_ = b;
        }
        if (forceKey_.exchange(false)) setCodec(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        auto* f = static_cast<WinFrame*>(frame.image.get());
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), f->texture.Get(), 0, FALSE, &buffer)) ||
            FAILED(MFCreateSample(&sample)))
            return true;
        sample->AddBuffer(buffer.Get());
        LONGLONG duration = 10'000'000 / fps_;
        sample->SetSampleTime(frameIndex_++ * duration);
        sample->SetSampleDuration(duration);
        transform_->ProcessInput(0, sample.Get(), 0);
        return true;
    }

    void drain() {
        MFT_OUTPUT_DATA_BUFFER db{};
        ComPtr<IMFSample> own;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(outSize_, &buffer);
            own->AddBuffer(buffer.Get());
            db.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &db, &status);
        if (db.pEvents) db.pEvents->Release();
        ComPtr<IMFSample> out;
        out.Attach(providesSamples_ ? db.pSample : own.Detach());
        if (FAILED(hr) || !out) return;
        UINT32 clean = 0;
        out->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        ComPtr<IMFMediaBuffer> contiguous;
        if (FAILED(out->ConvertToContiguousBuffer(&contiguous))) return;
        BYTE* p = nullptr;
        DWORD len = 0;
        contiguous->Lock(&p, nullptr, &len);
        auto split = splitParameterSets(ByteView(p, len));
        contiguous->Unlock();
        Clock::time_point captured = Clock::now();
        {
            std::scoped_lock l(m_);
            if (!captured_.empty()) {
                captured = captured_.front();
                captured_.pop_front();
            }
        }
        bool key = clean != 0;
        if (onFrame && !split.picture.empty())
            onFrame(std::move(split.picture), key, key ? std::move(split.config) : std::nullopt, captured);
    }

    int w_, h_, fps_;
    std::atomic<int> bitrate_;
    int appliedBitrate_ = 0;
    std::atomic<bool> forceKey_{true};
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<IMFMediaEventGenerator> events_;
    bool providesSamples_ = false;
    DWORD outSize_ = 0;
    LONGLONG frameIndex_ = 0;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::optional<Frame> pending_;
    std::deque<Clock::time_point> captured_;
};

} // namespace

bool hardwareEncoderAvailable(IDXGIAdapter* adapter) {
    DXGI_ADAPTER_DESC desc{};
    return adapter && SUCCEEDED(adapter->GetDesc(&desc)) && findEncoder(desc.AdapterLuid) != nullptr;
}

/// A hardware encoder on the frame's GPU, or null (the caller then uses the software one).
std::unique_ptr<Encoder> makeHardwareEncoder(ID3D11Device* device, int w, int h, int fps, int bitrate) {
    auto e = std::make_unique<HwEncoder>(w, h, fps, bitrate);
    if (!e->open(device)) return nullptr;
    log("H.264 encoder: hardware (Media Foundation)");
    return e;
}

} // namespace spanly::platform::win

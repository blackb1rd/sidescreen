// H.264 on Windows with Media Foundation's encoder (licensed with Windows): low-latency mode,
// constant bitrate, no B-frames, keyframes on request. Created on the capture thread (the first
// frame), where it then runs.
#include <initguid.h> // defines the CODECAPI_* GUIDs used below

#include "core/log.hpp"
#include "platform/platform.hpp"

#include <strmif.h> // ICodecAPI

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace spanly::platform {

using Microsoft::WRL::ComPtr;

namespace {

void setCodec(ICodecAPI* codec, const GUID& api, UINT32 value) {
    if (!codec) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    codec->SetValue(&api, &v);
}

class MfEncoder : public Encoder {
public:
    MfEncoder(int w, int h, int fps, int bitrate) : w_(w), h_(h), fps_(fps), bitrate_(bitrate) {}

    Codec codec() const override { return Codec::H264; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void requestKeyframe() override { forceKey_ = true; }
    void setBitrate(int bps) override { bitrate_ = bps; }

    void encode(const Frame& frame) override {
        if (frame.width != w_ || frame.height != h_) return; // a resize is settling
        if (!transform_ && !open()) return;
        if (int b = bitrate_; b != appliedBitrate_) {
            setCodec(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(b));
            appliedBitrate_ = b;
        }
        if (forceKey_.exchange(false)) setCodec(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        const auto& nv12 = *static_cast<const Bytes*>(frame.image.get());
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(DWORD(nv12.size()), &buffer))) return;
        BYTE* p = nullptr;
        buffer->Lock(&p, nullptr, nullptr);
        std::memcpy(p, nv12.data(), nv12.size());
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(nv12.size()));
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        LONGLONG duration = 10'000'000 / fps_; // 100 ns units
        sample->SetSampleTime(frameIndex_ * duration);
        sample->SetSampleDuration(duration);
        ++frameIndex_;
        if (FAILED(transform_->ProcessInput(0, sample.Get(), 0))) return;
        drain(frame.captured);
    }

private:
    bool open() {
        MFStartup(MF_VERSION, MFSTARTUP_FULL);
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &list,
                  &count);
        if (count > 0) list[0]->ActivateObject(IID_PPV_ARGS(&transform_));
        for (UINT32 i = 0; i < count; ++i)
            list[i]->Release();
        CoTaskMemFree(list);
        if (!transform_) {
            log("no H.264 encoder (Media Foundation) available");
            return false;
        }
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
        if (FAILED(transform_->SetOutputType(0, o.Get(), 0)) || FAILED(transform_->SetInputType(0, i.Get(), 0))) {
            log("H.264 encoder: {}x{} not supported", w_, h_);
            transform_.Reset();
            return false;
        }
        transform_.As(&codec_);
        setCodec(codec_.Get(), CODECAPI_AVLowLatencyMode, 1);
        setCodec(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setCodec(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(int(bitrate_)));
        setCodec(codec_.Get(), CODECAPI_AVEncMPVGOPSize, UINT32(fps_ * 10));
        setCodec(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        appliedBitrate_ = bitrate_;
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(0, &info);
        outSize_ = std::max<DWORD>(info.cbSize, DWORD(w_ * h_));
        log("H.264 encoder: Media Foundation");
        return true;
    }

    void drain(Clock::time_point captured) {
        while (true) {
            ComPtr<IMFSample> out;
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateSample(&out);
            MFCreateMemoryBuffer(outSize_, &buffer);
            out->AddBuffer(buffer.Get());
            MFT_OUTPUT_DATA_BUFFER db{0, out.Get(), 0, nullptr};
            DWORD status = 0;
            HRESULT hr = transform_->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) db.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) return;
            UINT32 clean = 0;
            out->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            ComPtr<IMFMediaBuffer> contiguous;
            out->ConvertToContiguousBuffer(&contiguous);
            BYTE* p = nullptr;
            DWORD len = 0;
            contiguous->Lock(&p, nullptr, &len);
            auto split = splitParameterSets(ByteView(p, len));
            contiguous->Unlock();
            bool key = clean != 0;
            if (onFrame && !split.picture.empty())
                onFrame(std::move(split.picture), key, key ? std::move(split.config) : std::nullopt, captured);
        }
    }

    int w_, h_, fps_;
    std::atomic<int> bitrate_;
    int appliedBitrate_ = 0;
    std::atomic<bool> forceKey_{true};
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_;
    LONGLONG frameIndex_ = 0;
    DWORD outSize_ = 0;
};

} // namespace

std::unique_ptr<Encoder> Encoder::create(int width, int height, int fps, int bitrate, Codec codec) {
    if (codec != Codec::H264) return nullptr;
    return std::make_unique<MfEncoder>(width, height, fps, bitrate);
}

} // namespace spanly::platform

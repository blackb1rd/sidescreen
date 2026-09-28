// BGRA -> NV12 with scaling on the GPU, using the D3D11 video processor (see d3d.hpp).
#include "core/log.hpp"
#include "platform/windows/d3d.hpp"

namespace spanly::platform::win {

std::unique_ptr<GpuConverter> GpuConverter::create(ID3D11Device* device, int srcW, int srcH, int dstW, int dstH) {
    std::unique_ptr<GpuConverter> c(new GpuConverter());
    c->device_ = device;
    device->GetImmediateContext(&c->context_);
    if (FAILED(c->device_.As(&c->video_)) || FAILED(c->context_.As(&c->videoContext_))) return nullptr;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = UINT(srcW);
    cd.InputHeight = UINT(srcH);
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = UINT(dstW);
    cd.OutputHeight = UINT(dstH);
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(c->video_->CreateVideoProcessorEnumerator(&cd, &c->enumerator_)) ||
        FAILED(c->video_->CreateVideoProcessor(c->enumerator_.Get(), 0, &c->processor_)))
        return nullptr;
    UINT support = 0;
    if (FAILED(c->enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        return nullptr;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(srcW);
    d.Height = UINT(srcH);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &c->input_))) return nullptr;
    d.Width = UINT(dstW);
    d.Height = UINT(dstH);
    d.Format = DXGI_FORMAT_NV12;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& t : c->pool_) {
        if (FAILED(device->CreateTexture2D(&d, nullptr, &t))) return nullptr;
    }
    // Screen content is full-range RGB; the encoder expects BT.709 limited-range YUV.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
    in.RGB_Range = 0;     // full
    out.YCbCr_Matrix = 1; // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    c->videoContext_->VideoProcessorSetStreamColorSpace(c->processor_.Get(), 0, &in);
    c->videoContext_->VideoProcessorSetOutputColorSpace(c->processor_.Get(), &out);
    c->srcW_ = srcW;
    c->srcH_ = srcH;
    c->dstW_ = dstW;
    c->dstH_ = dstH;
    return c;
}

ComPtr<ID3D11Texture2D> GpuConverter::convert(ID3D11Texture2D* bgra) {
    context_->CopyResource(input_.Get(), bgra);
    ComPtr<ID3D11Texture2D> out = pool_[next_];
    next_ = (next_ + 1) % pool_.size();

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(video_->CreateVideoProcessorInputView(input_.Get(), enumerator_.Get(), &iv, &inputView)) ||
        FAILED(video_->CreateVideoProcessorOutputView(out.Get(), enumerator_.Get(), &ov, &outputView)))
        return nullptr;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    if (FAILED(videoContext_->VideoProcessorBlt(processor_.Get(), outputView.Get(), 0, 1, &stream))) return nullptr;
    return out;
}

} // namespace spanly::platform::win

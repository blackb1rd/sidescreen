#pragma once
// The Windows GPU path: frames stay on the GPU from capture to the hardware encoder.

#include "core/protocol.hpp"
#include "platform/platform.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>

namespace spanly::platform::win {

using Microsoft::WRL::ComPtr;

/// What Frame::image holds on Windows: an NV12 texture (GPU path) or NV12 bytes (CPU path).
struct WinFrame {
    ComPtr<ID3D11Texture2D> texture;
    Bytes nv12;
};

/// BGRA -> NV12 with scaling, on the GPU (the D3D11 video processor). Capture thread only.
class GpuConverter {
public:
    /// Null if the GPU has no video processor for this conversion.
    static std::unique_ptr<GpuConverter> create(ID3D11Device* device, int srcW, int srcH, int dstW, int dstH);
    /// Convert into the next texture of a small pool (the encoder may still read the previous ones).
    ComPtr<ID3D11Texture2D> convert(ID3D11Texture2D* bgra);
    int srcW() const { return srcW_; }
    int srcH() const { return srcH_; }
    int dstW() const { return dstW_; }
    int dstH() const { return dstH_; }

private:
    GpuConverter() = default;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> input_; // our copy of the captured picture (the duplication's is released at once)
    std::array<ComPtr<ID3D11Texture2D>, 6> pool_;
    size_t next_ = 0;
    int srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0;
};

/// Whether a hardware H.264 encoder (Media Foundation) exists on this adapter.
bool hardwareEncoderAvailable(IDXGIAdapter* adapter);
/// A hardware encoder on `device`'s GPU, or null.
std::unique_ptr<Encoder> makeHardwareEncoder(ID3D11Device* device, int w, int h, int fps, int bitrate);

} // namespace spanly::platform::win

// Capture on Windows: DXGI Desktop Duplication. Frames arrive only when the screen changes,
// already on the GPU. With a hardware encoder on that GPU they stay there: the video processor
// converts them to NV12 at the stream size (the GPU path). Otherwise they're copied to the CPU
// and converted there for the software encoder.
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/win.hpp"

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace spanly::platform {

using Microsoft::WRL::ComPtr;

namespace {

/// BGRA -> NV12 (BT.601 limited range), nearest-neighbour scaled to w x h.
void bgraToNv12(const uint8_t* src, int sw, int sh, int pitch, int w, int h, Bytes& out) {
    out.resize(size_t(w) * size_t(h) * 3 / 2);
    uint8_t* yPlane = out.data();
    uint8_t* uv = out.data() + size_t(w) * size_t(h);
    auto px = [&](int x, int y, int& r, int& g, int& b) {
        const uint8_t* p = src + size_t(y * sh / h) * size_t(pitch) + size_t(x * sw / w) * 4;
        b = p[0];
        g = p[1];
        r = p[2];
    };
    int r = 0, g = 0, b = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            px(x, y, r, g, b);
            yPlane[size_t(y) * size_t(w) + size_t(x)] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            px(x, y, r, g, b);
            size_t i = size_t(y / 2) * size_t(w) + size_t(x);
            uv[i] = uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[i + 1] = uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

struct Duplicator {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingW = 0, stagingH = 0;
    bool gpu = false; // the GPU path (see the top of the file)
    std::unique_ptr<win::GpuConverter> converter;

    bool open(const std::wstring& deviceName) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc;
                output->GetDesc(&desc);
                if (deviceName != desc.DeviceName) continue;
                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output.As(&output1))) return false;
                if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                             nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
                    return false;
                if (ComPtr<ID3D10Multithread> mt; SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
                gpu = win::hardwareEncoderAvailable(adapter.Get());
                return SUCCEEDED(output1->DuplicateOutput(device.Get(), &dup));
            }
        }
        return false;
    }

    /// Wait up to `ms` for a changed picture: 1 = new picture in `out`, 0 = nothing changed, -1 = lost.
    int next(UINT ms, int w, int h, win::WinFrame& out) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dup->AcquireNextFrame(ms, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
        if (FAILED(hr)) return -1;
        if (info.LastPresentTime.QuadPart == 0) { // only the mouse moved
            dup->ReleaseFrame();
            return 0;
        }
        ComPtr<ID3D11Texture2D> tex;
        resource.As(&tex);
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (gpu) {
            if (!converter || converter->srcW() != int(d.Width) || converter->srcH() != int(d.Height) ||
                converter->dstW() != w || converter->dstH() != h)
                converter = win::GpuConverter::create(device.Get(), int(d.Width), int(d.Height), w, h);
            if (converter) {
                out.texture = converter->convert(tex.Get());
                dup->ReleaseFrame();
                return out.texture ? 1 : 0;
            }
            gpu = false; // no video processor for this: the CPU path from now on
            log("screen capture: converting on the CPU (the GPU's video processor isn't available)");
        }
        if (!staging || stagingW != d.Width || stagingH != d.Height) {
            D3D11_TEXTURE2D_DESC sd = d;
            sd.MipLevels = sd.ArraySize = 1;
            sd.SampleDesc = {1, 0};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            staging.Reset();
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) {
                dup->ReleaseFrame();
                return -1;
            }
            stagingW = d.Width;
            stagingH = d.Height;
        }
        context->CopyResource(staging.Get(), tex.Get());
        dup->ReleaseFrame();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1;
        bgraToNv12(static_cast<const uint8_t*>(mapped.pData), int(d.Width), int(d.Height), int(mapped.RowPitch), w, h,
                   out.nv12);
        context->Unmap(staging.Get(), 0);
        return 1;
    }
};

class DxgiCapture : public Capture {
public:
    ~DxgiCapture() override { halt(); }

    void start(DisplayId id, int width, int height, int fps, bool,
               std::function<void(const std::string&)> done) override {
        halt();
        std::wstring device = displayDevice(id);
        if (device.empty()) return done("display not found");
        w_ = width;
        h_ = height;
        running_ = true;
        thread_ = std::thread([this, device, fps, done = std::move(done)] { loop(device, fps, done); });
    }

    void resize(int width, int height) override {
        w_ = width;
        h_ = height;
    }
    void setShowsCursor(bool) override {} // Desktop Duplication leaves the cursor out

    void stop(std::function<void()> done) override {
        halt();
        done();
    }

    void resendLast() override {
        std::optional<Frame> f;
        {
            std::scoped_lock l(m_);
            f = last_;
        }
        if (f && onFrame) {
            f->captured = Clock::now();
            onFrame(*f);
        }
    }

private:
    void halt() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        std::scoped_lock l(m_);
        last_.reset();
    }

    void loop(const std::wstring& device, int fps, const std::function<void(const std::string&)>& done) {
        win::ComScope com; // the encoder runs on this thread too
        Duplicator dup;
        if (!dup.open(device)) {
            running_ = false;
            done("screen capture (DXGI) could not start");
            return;
        }
        done("");
        auto interval = std::chrono::microseconds(1'000'000 / std::max(fps, 1));
        auto last = Clock::now() - interval;
        while (running_) {
            int w = w_, h = h_;
            auto buffer = std::make_shared<win::WinFrame>();
            int r = dup.next(100, w, h, *buffer);
            if (r < 0) { // e.g. the resolution changed or a full-screen app took over
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                dup = Duplicator{};
                if (!dup.open(device)) log("screen capture interrupted; retrying");
                continue;
            }
            if (r == 0 || Clock::now() - last < interval) continue;
            last = Clock::now();
            Frame f{std::static_pointer_cast<void>(buffer), w, h, last};
            {
                std::scoped_lock l(m_);
                last_ = f;
            }
            if (onFrame) onFrame(f);
        }
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> w_{0}, h_{0};
    std::mutex m_;
    std::optional<Frame> last_;
};

} // namespace

std::unique_ptr<Capture> Capture::create() {
    return std::make_unique<DxgiCapture>();
}

} // namespace spanly::platform

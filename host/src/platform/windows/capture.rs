//! Screen capture with DXGI Desktop Duplication: frames arrive only when the screen changes,
//! already on the GPU; they're copied to a CPU texture and converted to NV12 at the stream size.

use crate::backend::Mode;
use windows::Win32::Foundation::{HMODULE, RECT};
use windows::Win32::Graphics::Direct3D::D3D_DRIVER_TYPE_UNKNOWN;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC;
use windows::Win32::Graphics::Dxgi::*;
use windows::Win32::Graphics::Gdi::{
    CDS_TYPE, ChangeDisplaySettingsExW, DEVMODEW, DISPLAY_DEVICEW, DM_PELSHEIGHT, DM_PELSWIDTH,
    EnumDisplayDevicesW,
};
use windows::core::{Interface, PCWSTR, Result};

pub struct Output {
    adapter: IDXGIAdapter1,
    output: IDXGIOutput1,
    pub rect: RECT,
    pub device_name: Vec<u16>,
    pub description: String,
    pub primary: bool,
}

fn monitor_description(device: &[u16]) -> String {
    unsafe {
        let mut dd = DISPLAY_DEVICEW {
            cb: std::mem::size_of::<DISPLAY_DEVICEW>() as u32,
            ..Default::default()
        };
        if EnumDisplayDevicesW(PCWSTR(device.as_ptr()), 0, &mut dd, 0).as_bool() {
            let end = dd
                .DeviceString
                .iter()
                .position(|&c| c == 0)
                .unwrap_or(dd.DeviceString.len());
            return String::from_utf16_lossy(&dd.DeviceString[..end]);
        }
    }
    String::new()
}

pub fn outputs() -> Result<Vec<Output>> {
    let mut list = Vec::new();
    unsafe {
        let factory: IDXGIFactory1 = CreateDXGIFactory1()?;
        let mut a = 0;
        while let Ok(adapter) = factory.EnumAdapters1(a) {
            let mut o = 0;
            while let Ok(output) = adapter.EnumOutputs(o) {
                o += 1;
                let desc = output.GetDesc()?;
                if !desc.AttachedToDesktop.as_bool() {
                    continue;
                }
                let end = desc
                    .DeviceName
                    .iter()
                    .position(|&c| c == 0)
                    .unwrap_or(desc.DeviceName.len());
                let mut device_name = desc.DeviceName[..end].to_vec();
                device_name.push(0);
                let r = desc.DesktopCoordinates;
                list.push(Output {
                    adapter: adapter.clone(),
                    output: output.cast()?,
                    rect: r,
                    description: monitor_description(&device_name),
                    device_name,
                    primary: r.left == 0 && r.top == 0,
                });
            }
            a += 1;
        }
    }
    Ok(list)
}

/// Mirror: the primary monitor. Extend: the virtual monitor (Virtual Display Driver).
pub fn pick(mode: Mode) -> std::result::Result<Output, String> {
    let mut list = outputs().map_err(|e| format!("could not list monitors: {e}"))?;
    let index = match mode {
        Mode::Mirror => list.iter().position(|o| o.primary),
        Mode::Extend => {
            let virtual_like = |o: &Output| {
                let d = o.description.to_lowercase();
                !o.primary && ["virtual", "vdd", "idd"].iter().any(|k| d.contains(k))
            };
            list.iter().position(virtual_like).or_else(|| {
                let secondary: Vec<usize> = (0..list.len()).filter(|&i| !list[i].primary).collect();
                (secondary.len() == 1).then(|| secondary[0])
            })
        }
    };
    let Some(i) = index else {
        return Err(match mode {
            Mode::Mirror => "no primary monitor found".into(),
            Mode::Extend => "no virtual monitor found: install the Virtual Display Driver (see the README) or use --mirror".into(),
        });
    };
    let o = list.swap_remove(i);
    log::info!(
        "capturing {} ({}x{})",
        o.description,
        o.rect.right - o.rect.left,
        o.rect.bottom - o.rect.top
    );
    Ok(o)
}

/// Best effort: give the (virtual) monitor the tablet's resolution.
pub fn set_resolution(o: &Output, width: u32, height: u32) {
    unsafe {
        let mode = DEVMODEW {
            dmSize: std::mem::size_of::<DEVMODEW>() as u16,
            dmPelsWidth: width,
            dmPelsHeight: height,
            dmFields: DM_PELSWIDTH | DM_PELSHEIGHT,
            ..Default::default()
        };
        let r = ChangeDisplaySettingsExW(
            PCWSTR(o.device_name.as_ptr()),
            Some(&mode),
            None,
            CDS_TYPE(0),
            None,
        );
        log::info!("set {} to {width}x{height}: {r:?}", o.description);
    }
}

pub struct Duplicator {
    device: ID3D11Device,
    context: ID3D11DeviceContext,
    dup: IDXGIOutputDuplication,
    staging: Option<(ID3D11Texture2D, u32, u32)>,
}

impl Duplicator {
    pub fn new(o: &Output) -> Result<Self> {
        unsafe {
            let (mut device, mut context) = (None, None);
            D3D11CreateDevice(
                &o.adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                HMODULE::default(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                None,
                D3D11_SDK_VERSION,
                Some(&mut device),
                None,
                Some(&mut context),
            )?;
            let device = device.unwrap();
            let dup = o.output.DuplicateOutput(&device)?;
            Ok(Self {
                device,
                context: context.unwrap(),
                dup,
                staging: None,
            })
        }
    }

    /// Wait up to `timeout_ms` for a changed picture and hand its BGRA pixels to `f`
    /// (data, width, height, row pitch). Ok(false) when nothing changed.
    pub fn next(&mut self, timeout_ms: u32, f: impl FnOnce(&[u8], u32, u32, u32)) -> Result<bool> {
        unsafe {
            let mut info = DXGI_OUTDUPL_FRAME_INFO::default();
            let mut res: Option<IDXGIResource> = None;
            if let Err(e) = self.dup.AcquireNextFrame(timeout_ms, &mut info, &mut res) {
                return if e.code() == DXGI_ERROR_WAIT_TIMEOUT {
                    Ok(false)
                } else {
                    Err(e)
                };
            }
            if info.LastPresentTime == 0 {
                self.dup.ReleaseFrame()?; // only the mouse moved
                return Ok(false);
            }
            let tex: ID3D11Texture2D = res.unwrap().cast()?;
            let mut d = D3D11_TEXTURE2D_DESC::default();
            tex.GetDesc(&mut d);
            if self
                .staging
                .as_ref()
                .is_none_or(|s| (s.1, s.2) != (d.Width, d.Height))
            {
                let sd = D3D11_TEXTURE2D_DESC {
                    Width: d.Width,
                    Height: d.Height,
                    MipLevels: 1,
                    ArraySize: 1,
                    Format: d.Format,
                    SampleDesc: DXGI_SAMPLE_DESC {
                        Count: 1,
                        Quality: 0,
                    },
                    Usage: D3D11_USAGE_STAGING,
                    BindFlags: 0,
                    CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                    MiscFlags: 0,
                };
                let mut t = None;
                self.device.CreateTexture2D(&sd, None, Some(&mut t))?;
                self.staging = Some((t.unwrap(), d.Width, d.Height));
            }
            let staging = &self.staging.as_ref().unwrap().0;
            self.context.CopyResource(staging, &tex);
            self.dup.ReleaseFrame()?;
            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            self.context
                .Map(staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))?;
            let data = std::slice::from_raw_parts(
                mapped.pData as *const u8,
                (mapped.RowPitch * d.Height) as usize,
            );
            f(data, d.Width, d.Height, mapped.RowPitch);
            self.context.Unmap(staging, 0);
            Ok(true)
        }
    }
}

/// BGRA -> NV12 (BT.601 limited range) with nearest-neighbour scaling to `w` x `h`.
pub fn bgra_to_nv12(src: &[u8], sw: u32, sh: u32, pitch: u32, w: u32, h: u32, out: &mut Vec<u8>) {
    let (w, h) = (w as usize, h as usize);
    out.resize(w * h * 3 / 2, 0);
    let (y_plane, uv) = out.split_at_mut(w * h);
    let px = |x: usize, y: usize| {
        let sx = x * sw as usize / w;
        let sy = y * sh as usize / h;
        let i = sy * pitch as usize + sx * 4;
        (src[i + 2] as i32, src[i + 1] as i32, src[i] as i32) // r, g, b
    };
    for y in 0..h {
        for x in 0..w {
            let (r, g, b) = px(x, y);
            y_plane[y * w + x] = (((66 * r + 129 * g + 25 * b + 128) >> 8) + 16) as u8;
        }
    }
    for y in (0..h).step_by(2) {
        for x in (0..w).step_by(2) {
            let (r, g, b) = px(x, y);
            let i = (y / 2) * w + x;
            uv[i] = (((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128) as u8;
            uv[i + 1] = (((112 * r - 94 * g - 18 * b + 128) >> 8) + 128) as u8;
        }
    }
}

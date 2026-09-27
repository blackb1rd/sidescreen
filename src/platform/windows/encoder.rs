//! H.264 with Media Foundation's encoder (licensed with Windows): low-latency mode, constant
//! bitrate, no B-frames, keyframes on request.

use std::mem::ManuallyDrop;
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::System::Variant::VARIANT;
use windows::core::{GUID, Interface, Result};

pub struct H264Encoder {
    transform: IMFTransform,
    codec: Option<ICodecAPI>,
    fps: u32,
    frame: i64,
    out_size: u32,
}

fn set(codec: &Option<ICodecAPI>, api: &GUID, value: VARIANT) {
    if let Some(c) = codec {
        unsafe {
            let _ = c.SetValue(api, &value);
        }
    }
}

impl H264Encoder {
    pub fn new(width: u32, height: u32, fps: u32, bitrate: u32) -> Result<Self> {
        unsafe {
            MFStartup(MF_VERSION, MFSTARTUP_FULL)?;
            let input = MFT_REGISTER_TYPE_INFO {
                guidMajorType: MFMediaType_Video,
                guidSubtype: MFVideoFormat_NV12,
            };
            let output = MFT_REGISTER_TYPE_INFO {
                guidMajorType: MFMediaType_Video,
                guidSubtype: MFVideoFormat_H264,
            };
            let mut list: *mut Option<IMFActivate> = std::ptr::null_mut();
            let mut count = 0u32;
            MFTEnumEx(
                MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                Some(&input),
                Some(&output),
                &mut list,
                &mut count,
            )?;
            let activates = std::slice::from_raw_parts_mut(list, count as usize);
            let transform: Option<IMFTransform> = activates
                .first()
                .and_then(|a| a.as_ref())
                .and_then(|a| a.ActivateObject().ok());
            for a in activates.iter_mut() {
                drop(a.take());
            }
            CoTaskMemFree(Some(list as *const _));
            let transform = transform
                .ok_or_else(|| windows::core::Error::from(windows::Win32::Foundation::E_FAIL))?;

            let size = ((width as u64) << 32) | height as u64;
            let rate = ((fps as u64) << 32) | 1;
            let out = MFCreateMediaType()?;
            out.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
            out.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_H264)?;
            out.SetUINT32(&MF_MT_AVG_BITRATE, bitrate)?;
            out.SetUINT64(&MF_MT_FRAME_SIZE, size)?;
            out.SetUINT64(&MF_MT_FRAME_RATE, rate)?;
            out.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
            out.SetUINT32(&MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main.0 as u32)?;
            transform.SetOutputType(0, &out, 0)?;
            let inp = MFCreateMediaType()?;
            inp.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
            inp.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_NV12)?;
            inp.SetUINT64(&MF_MT_FRAME_SIZE, size)?;
            inp.SetUINT64(&MF_MT_FRAME_RATE, rate)?;
            inp.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
            transform.SetInputType(0, &inp, 0)?;

            let codec: Option<ICodecAPI> = transform.cast().ok();
            set(&codec, &CODECAPI_AVLowLatencyMode, VARIANT::from(true));
            set(
                &codec,
                &CODECAPI_AVEncCommonRateControlMode,
                VARIANT::from(eAVEncCommonRateControlMode_CBR.0 as u32),
            );
            set(
                &codec,
                &CODECAPI_AVEncCommonMeanBitRate,
                VARIANT::from(bitrate),
            );
            set(&codec, &CODECAPI_AVEncMPVGOPSize, VARIANT::from(fps * 10));
            set(
                &codec,
                &CODECAPI_AVEncMPVDefaultBPictureCount,
                VARIANT::from(0u32),
            );

            transform.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)?;
            transform.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)?;
            let info = transform.GetOutputStreamInfo(0)?;
            Ok(Self {
                transform,
                codec,
                fps,
                frame: 0,
                out_size: info.cbSize.max(width * height),
            })
        }
    }

    pub fn force_keyframe(&self) {
        set(
            &self.codec,
            &CODECAPI_AVEncVideoForceKeyFrame,
            VARIANT::from(1u32),
        );
    }

    pub fn set_bitrate(&self, bps: u32) {
        set(
            &self.codec,
            &CODECAPI_AVEncCommonMeanBitRate,
            VARIANT::from(bps),
        );
    }

    /// Encode one NV12 picture; returns the access units that came out (Annex-B, keyframe?).
    pub fn encode(&mut self, nv12: &[u8]) -> Result<Vec<(Vec<u8>, bool)>> {
        unsafe {
            let buf = MFCreateMemoryBuffer(nv12.len() as u32)?;
            let mut ptr = std::ptr::null_mut();
            buf.Lock(&mut ptr, None, None)?;
            std::ptr::copy_nonoverlapping(nv12.as_ptr(), ptr, nv12.len());
            buf.Unlock()?;
            buf.SetCurrentLength(nv12.len() as u32)?;
            let sample = MFCreateSample()?;
            sample.AddBuffer(&buf)?;
            let duration = 10_000_000 / self.fps as i64; // 100 ns units
            sample.SetSampleTime(self.frame * duration)?;
            sample.SetSampleDuration(duration)?;
            self.frame += 1;
            self.transform.ProcessInput(0, &sample, 0)?;

            let mut out = Vec::new();
            loop {
                let osample = MFCreateSample()?;
                osample.AddBuffer(&MFCreateMemoryBuffer(self.out_size)?)?;
                let mut db = [MFT_OUTPUT_DATA_BUFFER {
                    dwStreamID: 0,
                    pSample: ManuallyDrop::new(Some(osample.clone())),
                    dwStatus: 0,
                    pEvents: ManuallyDrop::new(None),
                }];
                let mut status = 0u32;
                let r = self.transform.ProcessOutput(0, &mut db, &mut status);
                ManuallyDrop::drop(&mut db[0].pSample);
                ManuallyDrop::drop(&mut db[0].pEvents);
                match r {
                    Ok(()) => {
                        let key = osample
                            .GetUINT32(&MFSampleExtension_CleanPoint)
                            .unwrap_or(0)
                            != 0;
                        let b = osample.ConvertToContiguousBuffer()?;
                        let mut p = std::ptr::null_mut();
                        let mut len = 0u32;
                        b.Lock(&mut p, None, Some(&mut len))?;
                        let data = std::slice::from_raw_parts(p, len as usize).to_vec();
                        b.Unlock()?;
                        out.push((data, key));
                    }
                    Err(e) if e.code() == MF_E_TRANSFORM_NEED_MORE_INPUT => break,
                    Err(e) => return Err(e),
                }
            }
            Ok(out)
        }
    }
}

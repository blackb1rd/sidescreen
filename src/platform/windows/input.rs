//! Touches become mouse input with SendInput, positioned on the streamed monitor.

use crate::backend::Pointer;
use windows::Win32::Foundation::RECT;
use windows::Win32::UI::Input::KeyboardAndMouse::*;
use windows::Win32::UI::WindowsAndMessaging::{
    GetSystemMetrics, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN,
};

/// Wheel units per pixel of finger movement (120 = one notch, about three lines).
const WHEEL_PER_PIXEL: f32 = 2.0;

pub struct Input {
    /// The streamed monitor, in virtual-desktop coordinates.
    pub monitor: RECT,
    pressed: bool,
}

impl Input {
    pub fn new(monitor: RECT) -> Self {
        Self {
            monitor,
            pressed: false,
        }
    }

    fn mouse(flags: MOUSE_EVENT_FLAGS, dx: i32, dy: i32, data: i32) -> INPUT {
        INPUT {
            r#type: INPUT_MOUSE,
            Anonymous: INPUT_0 {
                mi: MOUSEINPUT {
                    dx,
                    dy,
                    mouseData: data as u32,
                    dwFlags: flags,
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        }
    }

    fn key(vk: VIRTUAL_KEY, up: bool) -> INPUT {
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: vk,
                    wScan: 0,
                    dwFlags: if up {
                        KEYEVENTF_KEYUP
                    } else {
                        KEYBD_EVENT_FLAGS(0)
                    },
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        }
    }

    fn send(inputs: &[INPUT]) {
        unsafe {
            SendInput(inputs, std::mem::size_of::<INPUT>() as i32);
        }
    }

    /// Absolute coordinates (0-65535 across the whole virtual desktop) for a point on the monitor.
    fn absolute(&self, x: f32, y: f32) -> (i32, i32) {
        let m = self.monitor;
        let px = m.left as f32 + x * (m.right - m.left) as f32;
        let py = m.top as f32 + y * (m.bottom - m.top) as f32;
        unsafe {
            let (vx, vy) = (
                GetSystemMetrics(SM_XVIRTUALSCREEN) as f32,
                GetSystemMetrics(SM_YVIRTUALSCREEN) as f32,
            );
            let (vw, vh) = (
                GetSystemMetrics(SM_CXVIRTUALSCREEN).max(1) as f32,
                GetSystemMetrics(SM_CYVIRTUALSCREEN).max(1) as f32,
            );
            (
                ((px - vx) * 65535.0 / vw) as i32,
                ((py - vy) * 65535.0 / vh) as i32,
            )
        }
    }

    fn move_to(&self, x: f32, y: f32) -> INPUT {
        let (ax, ay) = self.absolute(x, y);
        Self::mouse(
            MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
            ax,
            ay,
            0,
        )
    }

    pub fn pointer(&mut self, action: Pointer, x: f32, y: f32) {
        let mv = self.move_to(x, y);
        match action {
            Pointer::Down => {
                self.pressed = true;
                Self::send(&[mv, Self::mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 0)]);
            }
            Pointer::Up if self.pressed => {
                self.pressed = false;
                Self::send(&[mv, Self::mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0)]);
            }
            Pointer::RightClick => Self::send(&[
                mv,
                Self::mouse(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0),
                Self::mouse(MOUSEEVENTF_RIGHTUP, 0, 0, 0),
            ]),
            _ => Self::send(&[mv]),
        }
    }

    /// Content follows the fingers: fingers down = wheel up (positive), fingers right = wheel left.
    pub fn scroll(&mut self, x: f32, y: f32, dx: f32, dy: f32) {
        let m = self.monitor;
        let v = (dy * (m.bottom - m.top) as f32 * WHEEL_PER_PIXEL) as i32;
        let h = -(dx * (m.right - m.left) as f32 * WHEEL_PER_PIXEL) as i32;
        let mut inputs = vec![self.move_to(x, y)];
        if v != 0 {
            inputs.push(Self::mouse(MOUSEEVENTF_WHEEL, 0, 0, v));
        }
        if h != 0 {
            inputs.push(Self::mouse(MOUSEEVENTF_HWHEEL, 0, 0, h));
        }
        Self::send(&inputs);
    }

    /// Ctrl + plus / minus.
    pub fn zoom(&mut self, direction: i8) {
        let k = if direction > 0 {
            VK_OEM_PLUS
        } else {
            VK_OEM_MINUS
        };
        Self::send(&[
            Self::key(VK_CONTROL, false),
            Self::key(k, false),
            Self::key(k, true),
            Self::key(VK_CONTROL, true),
        ]);
    }
}

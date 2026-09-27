//! xdg-desktop-portal: one RemoteDesktop session that also casts the screen. The user approves
//! it once in the desktop's dialog; the restore token saved in the config skips the dialog
//! next time. Works on GNOME and KDE, Wayland and X11, without root.

use crate::backend::{Mode, Pointer};
use crate::config::Config;
use ashpd::desktop::PersistMode;
use ashpd::desktop::remote_desktop::{
    DeviceType, KeyState, NotifyPointerAxisOptions, RemoteDesktop, SelectDevicesOptions,
};
use ashpd::desktop::screencast::{CursorMode, Screencast, SelectSourcesOptions, SourceType};
use std::os::fd::OwnedFd;
use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender, unbounded_channel};

/// Linux evdev codes.
const BTN_LEFT: i32 = 0x110;
const BTN_RIGHT: i32 = 0x111;
const KEY_LEFTCTRL: i32 = 29;
const KEY_EQUAL: i32 = 13;
const KEY_MINUS: i32 = 12;

pub enum Input {
    Pointer(Pointer, f32, f32),
    Scroll(f32, f32, f32, f32),
    Zoom(i8),
}

/// A running portal session: where to read the picture, and where to send input.
pub struct Cast {
    pub fd: OwnedFd,
    pub node: u32,
    /// The stream's size in the compositor's logical pixels (for pointer positions).
    pub size: (i32, i32),
    pub input: UnboundedSender<Input>,
}

/// Ask for the screen (a new virtual monitor in extend mode) plus pointer and keyboard control.
/// Keeps running on `rt` to forward input until the returned sender is dropped.
pub fn start(rt: &tokio::runtime::Runtime, mode: Mode) -> Result<Cast, String> {
    let (tx, rx) = unbounded_channel();
    let (ready_tx, ready_rx) = std::sync::mpsc::channel();
    rt.spawn(async move {
        match open(mode).await {
            Ok((rd, session, cast)) => {
                let node = cast.1;
                let _ = ready_tx.send(Ok((cast.0, node, cast.2)));
                forward_input(rd, session, node, cast.2, rx).await;
            }
            Err(e) => {
                let _ = ready_tx.send(Err(e));
            }
        }
    });
    let (fd, node, size) = ready_rx
        .recv()
        .map_err(|_| "portal task ended".to_string())??;
    Ok(Cast {
        fd,
        node,
        size,
        input: tx,
    })
}

type Session = ashpd::desktop::Session<RemoteDesktop>;

async fn open(mode: Mode) -> Result<(RemoteDesktop, Session, (OwnedFd, u32, (i32, i32))), String> {
    let err = |e: ashpd::Error| format!("desktop portal: {e}");
    let rd = RemoteDesktop::new().await.map_err(err)?;
    let sc = Screencast::new().await.map_err(err)?;
    let session = rd.create_session(Default::default()).await.map_err(err)?;
    let token = Config::load()
        .get("portal_restore_token")
        .map(str::to_string);

    rd.select_devices(
        &session,
        SelectDevicesOptions::default()
            .set_devices(DeviceType::Pointer | DeviceType::Keyboard)
            .set_persist_mode(PersistMode::ExplicitlyRevoked)
            .set_restore_token(token.as_deref()),
    )
    .await
    .map_err(err)?;
    let source = if mode == Mode::Extend {
        SourceType::Virtual
    } else {
        SourceType::Monitor
    };
    sc.select_sources(
        &session,
        SelectSourcesOptions::default()
            .set_cursor_mode(CursorMode::Embedded)
            .set_sources(ashpd::enumflags2::BitFlags::from_flag(source))
            .set_multiple(false),
    )
    .await
    .map_err(|e| {
        format!(
            "{} (a virtual monitor needs GNOME 46+ or KDE Plasma 6; try --mirror)",
            err(e)
        )
    })?;

    let started = rd
        .start(&session, None, Default::default())
        .await
        .map_err(err)?
        .response()
        .map_err(err)?;
    if let Some(t) = started.restore_token() {
        Config::load().set("portal_restore_token", Some(t));
    }
    let stream = started
        .streams()
        .first()
        .ok_or("the desktop didn't share a screen")?;
    let node = stream.pipe_wire_node_id();
    let size = stream.size().unwrap_or((1920, 1080));
    let fd = sc
        .open_pipe_wire_remote(&session, Default::default())
        .await
        .map_err(err)?;
    log::info!(
        "desktop portal: sharing PipeWire node {node}, {}x{}",
        size.0,
        size.1
    );
    Ok((rd, session, (fd, node, size)))
}

async fn forward_input(
    rd: RemoteDesktop,
    session: Session,
    node: u32,
    size: (i32, i32),
    mut rx: UnboundedReceiver<Input>,
) {
    let (w, h) = (size.0 as f64, size.1 as f64);
    let mut pressed = false;
    while let Some(input) = rx.recv().await {
        let r = match input {
            Input::Pointer(action, x, y) => {
                let (px, py) = (x as f64 * w, y as f64 * h);
                let moved = rd
                    .notify_pointer_motion_absolute(&session, node, px, py, Default::default())
                    .await;
                match action {
                    Pointer::Down => {
                        pressed = true;
                        rd.notify_pointer_button(
                            &session,
                            BTN_LEFT,
                            KeyState::Pressed,
                            Default::default(),
                        )
                        .await
                    }
                    Pointer::Up if pressed => {
                        pressed = false;
                        rd.notify_pointer_button(
                            &session,
                            BTN_LEFT,
                            KeyState::Released,
                            Default::default(),
                        )
                        .await
                    }
                    Pointer::RightClick => {
                        let _ = rd
                            .notify_pointer_button(
                                &session,
                                BTN_RIGHT,
                                KeyState::Pressed,
                                Default::default(),
                            )
                            .await;
                        rd.notify_pointer_button(
                            &session,
                            BTN_RIGHT,
                            KeyState::Released,
                            Default::default(),
                        )
                        .await
                    }
                    _ => moved,
                }
            }
            Input::Scroll(x, y, dx, dy) => {
                let _ = rd
                    .notify_pointer_motion_absolute(
                        &session,
                        node,
                        x as f64 * w,
                        y as f64 * h,
                        Default::default(),
                    )
                    .await;
                // Portal axis values scroll the view; content following the fingers is the opposite sign.
                rd.notify_pointer_axis(
                    &session,
                    -(dx as f64) * w,
                    -(dy as f64) * h,
                    NotifyPointerAxisOptions::default(),
                )
                .await
            }
            Input::Zoom(dir) => {
                let key = if dir > 0 { KEY_EQUAL } else { KEY_MINUS };
                let _ = rd
                    .notify_keyboard_keycode(
                        &session,
                        KEY_LEFTCTRL,
                        KeyState::Pressed,
                        Default::default(),
                    )
                    .await;
                let _ = rd
                    .notify_keyboard_keycode(&session, key, KeyState::Pressed, Default::default())
                    .await;
                let _ = rd
                    .notify_keyboard_keycode(&session, key, KeyState::Released, Default::default())
                    .await;
                rd.notify_keyboard_keycode(
                    &session,
                    KEY_LEFTCTRL,
                    KeyState::Released,
                    Default::default(),
                )
                .await
            }
        };
        if let Err(e) = r {
            log::debug!("desktop portal input: {e}");
        }
    }
    let _ = session.close().await;
}

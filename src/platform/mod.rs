//! Per-OS backends (see backend.rs).

#[cfg(target_os = "linux")]
pub mod linux;
#[cfg(feature = "test-pattern")]
pub mod test_pattern;
#[cfg(windows)]
pub mod windows;

//! 日志行拼装：按 IDF 默认格式拼出整行，再交给边界模块输出。
//! 拼行用固定缓冲，不分配堆内存；整行作为 "%s" 参数交给 C，消息里的 '%' 不会被当格式符。

use core::ffi::CStr;
use core::fmt::{self, Write};

use crate::boundary;

pub const LEVEL_ERROR: u32 = 1;
pub const LEVEL_WARN: u32 = 2;
pub const LEVEL_INFO: u32 = 3;

/// 拼行缓冲：超出部分截断，末尾始终留一个 '\0' 作为 C 字符串结尾。
const LINE_CAPACITY: usize = 192;

const RESET: &str = "\u{1b}[0m";
const COLOR_ERROR: &str = "\u{1b}[0;31m";
const COLOR_WARN: &str = "\u{1b}[0;33m";
const COLOR_INFO: &str = "\u{1b}[0;32m";

struct Line {
  buf: [u8; LINE_CAPACITY],
  len: usize,
}

impl Line {
  fn new() -> Self {
    Self {
      buf: [0; LINE_CAPACITY],
      len: 0,
    }
  }

  fn as_c_str(&self) -> Option<&CStr> {
    CStr::from_bytes_until_nul(&self.buf).ok()
  }
}

impl Write for Line {
  fn write_str(&mut self, text: &str) -> fmt::Result {
    let bytes = text.as_bytes();
    let free = LINE_CAPACITY - 1 - self.len;
    let count = free.min(bytes.len());
    self.buf[self.len..self.len + count].copy_from_slice(&bytes[..count]);
    self.len += count;
    Ok(())
  }
}

/// 输出一行日志（level 同时用于 IDF 的日志过滤）。
pub fn write(level: u32, tag: &CStr, args: fmt::Arguments) {
  let (color, letter) = match level {
    LEVEL_ERROR => (COLOR_ERROR, 'E'),
    LEVEL_WARN => (COLOR_WARN, 'W'),
    _ => (COLOR_INFO, 'I'),
  };
  let mut line = Line::new();
  let _ = fmt::write(
    &mut line,
    format_args!(
      "{color}{letter} ({}): {}: ",
      boundary::log_timestamp(),
      tag.to_str().unwrap_or("slint_ui")
    ),
  );
  let _ = fmt::write(&mut line, args);
  let _ = fmt::write(&mut line, format_args!("{RESET}\n"));
  if let Some(text) = line.as_c_str() {
    boundary::log_write(level, tag, text);
  }
}

macro_rules! log_info {
    ($tag:expr, $($arg:tt)*) => {
        $crate::log::write($crate::log::LEVEL_INFO, $tag, format_args!($($arg)*))
    };
}

macro_rules! log_warn {
    ($tag:expr, $($arg:tt)*) => {
        $crate::log::write($crate::log::LEVEL_WARN, $tag, format_args!($($arg)*))
    };
}

macro_rules! log_error {
    ($tag:expr, $($arg:tt)*) => {
        $crate::log::write($crate::log::LEVEL_ERROR, $tag, format_args!($($arg)*))
    };
}

pub(crate) use log_error;
pub(crate) use log_info;
pub(crate) use log_warn;

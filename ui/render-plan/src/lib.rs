//! 行带计划：把一帧里的一条 damage 矩形裁到视口内，再按行带高度切成交给面板的顺序。
//! 平台层（ui/slint_ui/src/platform.rs）按计划拷像素并同步提交；宿主用例按同一份实现
//! 断言切片下标与拷贝结果——下标错一处就是上屏撕裂或错位，宿主渲染器看不出这类缺陷。
//! 只依赖 core，不引用 Slint 类型：像素按泛型取，固件的 RGB565 与用例的合成缓冲都能进。

#![no_std]

/// 视口尺寸（像素）：整帧缓冲按它的宽度取每行行首。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Viewport {
  pub width: usize,
  pub height: usize,
}

/// 一条 damage 矩形（像素）：允许越出视口，切分时裁掉多出来的部分。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Damage {
  pub x: i32,
  pub y: i32,
  pub width: u32,
  pub height: u32,
}

/// 一条待提交的行带：已裁到视口内，行数不超过行带高度。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Band {
  pub x: usize,
  pub y: usize,
  pub width: usize,
  pub rows: usize,
}

/// 按行带高度切分一条 damage 矩形：先上后下，末段取余数。
/// 契约：裁完为空（越界到没有像素、宽或高为 0）时不产出条目；行带高度为 0 按 1 行处理。
pub fn bands(damage: Damage, viewport: Viewport, band_rows: usize) -> Bands {
  let left = damage.x.max(0) as i64;
  let top = damage.y.max(0) as i64;
  let right = damage.x as i64 + damage.width as i64;
  let bottom = damage.y as i64 + damage.height as i64;
  let width = (right.min(viewport.width as i64) - left).clamp(0, viewport.width as i64) as usize;
  let height = (bottom.min(viewport.height as i64) - top).clamp(0, viewport.height as i64) as usize;
  Bands {
    x: left.min(viewport.width as i64) as usize,
    y: top.min(viewport.height as i64) as usize,
    width: if left >= viewport.width as i64 { 0 } else { width },
    remaining: if top >= viewport.height as i64 { 0 } else { height },
    band_rows: band_rows.max(1),
  }
}

/// 行带切分的游标：bands() 返回它，逐条交出要提交的行带。
#[derive(Clone, Copy, Debug)]
pub struct Bands {
  x: usize,
  y: usize,
  width: usize,
  remaining: usize,
  band_rows: usize,
}

impl Iterator for Bands {
  type Item = Band;

  fn next(&mut self) -> Option<Band> {
    if self.remaining == 0 || self.width == 0 {
      return None;
    }
    let rows = self.remaining.min(self.band_rows);
    let band = Band {
      x: self.x,
      y: self.y,
      width: self.width,
      rows,
    };
    self.y += rows;
    self.remaining -= rows;
    Some(band)
  }
}

/// 把一条行带从整帧缓冲拷进行带缓冲：源按视口宽度逐行取，行带缓冲按行带宽度紧凑排布。
/// 返回这一段要提交的像素数（宽 × 行数），调用方按它切出提交用的那一段缓冲。
pub fn copy_band<P: Copy>(frame: &[P], band: &mut [P], viewport_width: usize, line: Band) -> usize {
  let mut cursor = 0;
  for row in 0..line.rows {
    let source = (line.y + row) * viewport_width + line.x;
    band[cursor..cursor + line.width].copy_from_slice(&frame[source..source + line.width]);
    cursor += line.width;
  }
  cursor
}

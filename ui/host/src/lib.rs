//! 宿主侧界面包：把 src/ 下的 .slint 编成 Rust，并给 #[test] 用例提供后端初始化、
//! 元素几何与画面量测工具（软件渲染器渲染到内存缓冲，与设备上的面板同一套渲染通路）。
//! 只在开发机上编译；固件里的同一份界面由同工作区的 fw 包交叉编译。

use std::collections::HashMap;

/// 设备侧界面：ui/src/app.slint 的生成代码（固件里编的是同一份源码）。
mod device {
    include!(concat!(env!("OUT_DIR"), "/app.rs"));
}
pub use device::*;

/// 预览窗：ui/preview.slint 的生成代码（设备画面 + 控制条，见该文件头部说明）。
mod preview {
    include!(concat!(env!("OUT_DIR"), "/preview.rs"));
}
pub use preview::PreviewApp;

pub use i_slint_backend_testing::{ElementHandle, ElementQuery};
pub use slint::{ComponentHandle, PhysicalSize, Rgba8Pixel, SharedPixelBuffer};

/// 面板尺寸（逻辑像素）：与硬件视口一致。
pub const SCREEN_WIDTH: u32 = 240;
pub const SCREEN_HEIGHT: u32 = 280;
/// 预览窗尺寸：设备画面（240 × 280）加下方控制条。
pub const PREVIEW_WIDTH: u32 = 240;
pub const PREVIEW_HEIGHT: u32 = 520;

/// 区域内算作墨迹的判据：与底色每通道的差都超过这个值。
const INK_TOLERANCE: u8 = 32;

/** 盒子：绝对位置 + 尺寸（逻辑像素）。 */
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
    pub fn center_x(&self) -> f32 {
        self.x + self.w / 2.0
    }

    pub fn center_y(&self) -> f32 {
        self.y + self.h / 2.0
    }

    pub fn right(&self) -> f32 {
        self.x + self.w
    }

    pub fn bottom(&self) -> f32 {
        self.y + self.h
    }

    /// 取整后的像素范围（左闭右开）：扫像素时用，含边界像素。
    pub fn pixel_bounds(&self) -> (i32, i32, i32, i32) {
        (
            self.x.floor() as i32,
            self.y.floor() as i32,
            self.right().ceil() as i32,
            self.bottom().ceil() as i32,
        )
    }
}

/** 一帧画面：RGBA8 像素，左上角为原点。 */
pub struct Frame {
    pixels: SharedPixelBuffer<Rgba8Pixel>,
}

impl Frame {
    pub fn width(&self) -> i32 {
        self.pixels.width() as i32
    }

    pub fn height(&self) -> i32 {
        self.pixels.height() as i32
    }

    pub fn rgb(&self, x: i32, y: i32) -> [u8; 3] {
        if x < 0 || y < 0 || x >= self.width() || y >= self.height() {
            return [0, 0, 0];
        }
        let p = self.pixels.as_slice()[(y * self.width() + x) as usize];
        [p.r, p.g, p.b]
    }

    /// 区域内落色为 rgb 的像素数（每通道容差 tol）：判断某块面有没有画出来。
    pub fn count_color(&self, rect: Rect, rgb: [u8; 3], tol: u8) -> usize {
        let (x0, y0, x1, y1) = rect.pixel_bounds();
        let mut hits = 0;
        for y in y0..y1 {
            for x in x0..x1 {
                if close(self.rgb(x, y), rgb, tol) {
                    hits += 1;
                }
            }
        }
        hits
    }

    /// 区域内非底色的像素点（底色取区域内出现最多的颜色）：找字形墨迹用。
    pub fn ink(&self, rect: Rect) -> Vec<(i32, i32)> {
        let (x0, y0, x1, y1) = rect.pixel_bounds();
        let mut counts: HashMap<[u8; 3], usize> = HashMap::new();
        for y in y0..y1 {
            for x in x0..x1 {
                *counts.entry(self.rgb(x, y)).or_default() += 1;
            }
        }
        let Some((&background, _)) = counts.iter().max_by_key(|(_, count)| **count) else {
            return Vec::new();
        };
        let mut points = Vec::new();
        for y in y0..y1 {
            for x in x0..x1 {
                if !close(self.rgb(x, y), background, INK_TOLERANCE) {
                    points.push((x, y));
                }
            }
        }
        points
    }

    /// 区域内墨迹的包围盒（x0, y0, x1, y1：左闭右开），没有墨迹则为 None。
    pub fn ink_bounds(&self, rect: Rect) -> Option<(i32, i32, i32, i32)> {
        let points = self.ink(rect);
        if points.is_empty() {
            return None;
        }
        let xs: Vec<i32> = points.iter().map(|p| p.0).collect();
        let ys: Vec<i32> = points.iter().map(|p| p.1).collect();
        Some((min(&xs), min(&ys), max(&xs) + 1, max(&ys) + 1))
    }

    /// 区域内出现过墨迹的列号：按列分段找字形用。
    pub fn ink_columns(&self, rect: Rect) -> Vec<i32> {
        let (x0, _, x1, _) = rect.pixel_bounds();
        let (_, y0, _, y1) = rect.pixel_bounds();
        let mut counts: HashMap<[u8; 3], usize> = HashMap::new();
        for y in y0..y1 {
            for x in x0..x1 {
                *counts.entry(self.rgb(x, y)).or_default() += 1;
            }
        }
        let Some((&background, _)) = counts.iter().max_by_key(|(_, count)| **count) else {
            return Vec::new();
        };
        (x0..x1)
            .filter(|x| (y0..y1).any(|y| !close(self.rgb(*x, y), background, INK_TOLERANCE)))
            .collect()
    }

    /// 区域内的墨迹按列分段（段间留一列以上的空白）：每段给出行内墨迹的包围盒。
    pub fn ink_runs(&self, rect: Rect) -> Vec<Rect> {
        let mut runs: Vec<Rect> = Vec::new();
        for x in self.ink_columns(rect) {
            match runs.last_mut() {
                Some(last) if x as f32 == last.right() => last.w += 1.0,
                _ => runs.push(Rect { x: x as f32, y: rect.y, w: 1.0, h: rect.h }),
            }
        }
        for run in runs.iter_mut() {
            if let Some((_, y0, _, y1)) = self.ink_bounds(*run) {
                run.y = y0 as f32;
                run.h = (y1 - y0) as f32;
            }
        }
        runs
    }
}

/// 区域内墨迹的水平中心（没有墨迹则为 None），用来判断图标与文字是否共用一条中线。
pub fn ink_center_x(frame: &Frame, rect: Rect) -> Option<f32> {
    frame.ink_bounds(rect).map(|(x0, _, x1, _)| (x0 + x1) as f32 / 2.0)
}

/// 区域内墨迹的竖直中心（没有墨迹则为 None）。
pub fn ink_center_y(frame: &Frame, rect: Rect) -> Option<f32> {
    frame.ink_bounds(rect).map(|(_, y0, _, y1)| (y0 + y1) as f32 / 2.0)
}

/// 装后端：Slint 测试后端 + 软件渲染器（取画面要真渲染器，测试后端默认只给假的量测）。
/// 每个用例跑在自己的线程上，各调一次即可。
pub fn init_backend() {
    let mut options = i_slint_backend_testing::TestingBackendOptions::default();
    options.mock_time = true;
    options.renderer_name = Some("software".into());
    slint::platform::set_platform(Box::new(i_slint_backend_testing::TestingBackend::new(options)))
        .expect("后端已经装过：一个用例只装一次");
}

/// 建一个按面板尺寸的窗口，属性停在 .slint 的默认值上（固件在运行期写这些属性）。
pub fn new_app() -> App {
    init_backend();
    let app = App::new().unwrap();
    app.show().unwrap();
    app.window().set_size(PhysicalSize::new(SCREEN_WIDTH, SCREEN_HEIGHT));
    app
}

/// 建 PC 预览窗：上半是设备画面，下半是控制条；动作由 PreviewApp 自己结算（见 ui/preview.slint）。
pub fn new_preview() -> PreviewApp {
    init_backend();
    let preview = PreviewApp::new().unwrap();
    preview.show().unwrap();
    preview.window().set_size(PhysicalSize::new(PREVIEW_WIDTH, PREVIEW_HEIGHT));
    preview
}

/// 按限定 id 取元素，id 写法是「组件名::id」，例如 BottomBar::battery-text。
/// 页面里的 id 在页面没实例化时取不到（if 块里的元素）。
pub fn element<C: ComponentHandle>(app: &C, id: &str) -> ElementHandle {
    element_or_none(app, id).unwrap_or_else(|| panic!("找不到元素 {id}：id 改名了，或元素没被实例化"))
}

/// 按限定 id 取元素，取不到给 None（元素在没实例化的 if 块里时就是这样）。
pub fn element_or_none<C: ComponentHandle>(app: &C, id: &str) -> Option<ElementHandle> {
    ElementHandle::find_by_element_id(app, id).next()
}

/// 元素盒子的绝对位置与尺寸。
pub fn rect<C: ComponentHandle>(app: &C, id: &str) -> Rect {
    let handle = element(app, id);
    let position = handle.absolute_position();
    let size = handle.size();
    Rect { x: position.x, y: position.y, w: size.width, h: size.height }
}

/// 当前画面：按窗口尺寸渲染一帧。
pub fn frame<C: ComponentHandle>(app: &C) -> Frame {
    let pixels = app.window().take_snapshot().expect("取画面失败：后端不是软件渲染器");
    Frame { pixels }
}

/// 推进模拟时间：动画与 Timer 只随后端推进的时间走（测试后端不自动流逝）。
pub fn elapse(duration: std::time::Duration) {
    i_slint_backend_testing::mock_elapsed_time(duration);
}

/// 等动效走完：推进时钟并渲染几帧，让滑入、提示箭头这类瞬时位移回到静止位置。
/// 切页后要量几何或画面时先调它，量到的才是稳定状态。
pub fn settle<C: ComponentHandle>(app: &C) {
    for _ in 0..6 {
        elapse(std::time::Duration::from_millis(50));
        let _ = app.window().take_snapshot();
    }
}

fn close(a: [u8; 3], b: [u8; 3], tol: u8) -> bool {
    let diff = |i: usize| (a[i] as i32 - b[i] as i32).unsigned_abs() as u8;
    diff(0) <= tol && diff(1) <= tol && diff(2) <= tol
}

fn min(values: &[i32]) -> i32 {
    *values.iter().min().unwrap()
}

fn max(values: &[i32]) -> i32 {
    *values.iter().max().unwrap()
}

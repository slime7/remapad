//! 无线调试页用例：WiFi 开关在右上角、正文随会话状态切换；开关同时接通网络手柄与日志。

use std::cell::RefCell;
use std::rc::Rc;

use remapad_ui as ui;

/// 无线调试页槽号：release 页表的末位（dev 构建在其后再追加调试页）。
const NETWORK_PAGE: i32 = 7;
/// 开关关：secondary-container 底色（theme.slint）。
const OFF_BG: [u8; 3] = [0x15, 0x2a, 0x1f];
/// 开关开：error-container 底色——会话在位挂 danger，与配对页的「停止 / 断开」同口径（theme.slint）。
const ON_BG: [u8; 3] = [0x8a, 0x1a, 0x1e];

/// 页面画出 WiFi 开关（唯一可聚焦项，落在右上角），点它发出 netlog-toggle 动作。
#[test]
fn 无线调试页的开关在右上角且点按发出开关动作() {
  let app = ui::new_app();
  let actions: Rc<RefCell<Vec<(String, i32)>>> = Rc::new(RefCell::new(Vec::new()));
  {
    let actions = actions.clone();
    app.on_action(move |name, value| actions.borrow_mut().push((name.to_string(), value)));
  }
  app.set_page(NETWORK_PAGE);
  ui::settle(&app);

  assert_eq!(app.get_focus_count(), 1, "无线调试页应只有 WiFi 开关一个可聚焦项");
  let button = ui::rect(&app, "NetworkPage::wifi-btn");
  assert!(
    button.center_x() > 150.0 && button.y < 80.0,
    "WiFi 开关没画在右上角：{button:?}"
  );

  ui::tap(&app, button.center_x(), button.center_y());
  assert!(
    actions.borrow().iter().any(|(name, _value)| name == "netlog-toggle"),
    "点 WiFi 开关没有发出 netlog-toggle：{:?}",
    actions.borrow()
  );
}

/// 正文随会话状态切换：关闭时是「未连接」、没有信号行；连上后换成 ip:port 并显示信号，开关底色也换挡。
#[test]
fn 无线调试页正文随会话状态切换() {
  let app = ui::new_app();
  app.set_page(NETWORK_PAGE);
  ui::settle(&app);

  assert!(
    ui::element_or_none(&app, "NetworkPage::rssi-line").is_none(),
    "未连接时不该有信号行"
  );
  let status = ui::rect(&app, "NetworkPage::status-line");
  let idle_width = match ui::frame(&app).ink_bounds(status) {
    Some((x0, _y0, x1, _y1)) => (x1 - x0) as f32,
    None => panic!("未连接时正文没有墨迹"),
  };

  app.set_netlog_state(2);
  app.set_netlog_addr("192.168.1.5:9999".into());
  app.set_netlog_rssi(-58);
  ui::settle(&app);

  let rssi = ui::rect(&app, "NetworkPage::rssi-line");
  let frame = ui::frame(&app);
  assert!(frame.ink_bounds(rssi).is_some(), "已连接时信号行没有墨迹");
  let addr_width = match frame.ink_bounds(status) {
    Some((x0, _y0, x1, _y1)) => (x1 - x0) as f32,
    None => panic!("已连接时正文没有墨迹"),
  };
  assert!(
    addr_width > idle_width + 20.0,
    "正文没有从「未连接」换成 ip:port（宽度 {addr_width:.1} 对 {idle_width:.1}）"
  );

  let button = ui::rect(&app, "NetworkPage::wifi-btn");
  assert!(
    frame.count_color(button, ON_BG, 8) * 2 > (button.w * button.h) as usize,
    "会话开启后开关底色没有换挡"
  );

  app.set_netlog_state(0);
  ui::settle(&app);
  assert!(
    ui::element_or_none(&app, "NetworkPage::rssi-line").is_none(),
    "关闭后信号行还在"
  );
  let frame = ui::frame(&app);
  assert!(
    frame.count_color(button, OFF_BG, 8) * 2 > (button.w * button.h) as usize,
    "关闭后开关底色没有切回"
  );
}

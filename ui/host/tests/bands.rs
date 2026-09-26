//! 行带计划用例：平台层把一帧的 damage 折成逐条提交的行带，切分与拷贝的真源码在
//! ui/render-plan（固件平台层与这里跑同一份实现），下标错一位就是上屏撕裂或错位。

use render_plan::{bands, copy_band, Band, Damage, Viewport};

/// 面板视口与行带高度：与 ui/slint_ui/src/platform.rs 的取值一致。
const VIEW: Viewport = Viewport {
  width: 240,
  height: 280,
};
const BAND_ROWS: usize = 48;

/// 合成一帧：每个像素写 y * 1000 + x，按内容就能看出取的是哪一行哪一列。
fn synthetic(view: Viewport) -> Vec<u32> {
  (0..view.height)
    .flat_map(|y| (0..view.width).map(move |x| (y * 1000 + x) as u32))
    .collect()
}

/// 整屏重画切成六条：前五条各 48 行，末条取余下的 40 行。
#[test]
fn 整屏重画按四十八行切分且末段取余数() {
  let full = Damage {
    x: 0,
    y: 0,
    width: VIEW.width as u32,
    height: VIEW.height as u32,
  };
  let plan: Vec<Band> = bands(full, VIEW, BAND_ROWS).collect();
  let shape: Vec<(usize, usize, usize)> = plan.iter().map(|b| (b.y, b.rows, b.width)).collect();
  assert_eq!(
    shape,
    vec![
      (0, 48, 240),
      (48, 48, 240),
      (96, 48, 240),
      (144, 48, 240),
      (192, 48, 240),
      (240, 40, 240),
    ],
    "整屏重画的行带切分不对"
  );
}

/// 不满宽的矩形：每一行都从整帧里的对应行取像素，行带缓冲里紧凑排布。
#[test]
fn 不满宽的矩形按自己的宽度逐行取像素() {
  let frame = synthetic(VIEW);
  let damage = Damage {
    x: 60,
    y: 100,
    width: 40,
    height: 60,
  };
  let mut band = vec![0u32; VIEW.width * BAND_ROWS];
  let mut submitted: Vec<Band> = Vec::new();

  for line in bands(damage, VIEW, BAND_ROWS) {
    let pixels = copy_band(&frame, &mut band, VIEW.width, line);
    assert_eq!(pixels, line.width * line.rows, "提交长度应是行带缓冲里那一段");
    for row in 0..line.rows {
      for col in 0..line.width {
        let want = ((line.y + row) * 1000 + line.x + col) as u32;
        assert_eq!(
          band[row * line.width + col],
          want,
          "第 {row} 行的第 {col} 列取错了像素：行带没按自己的宽度逐行取"
        );
      }
    }
    submitted.push(line);
  }

  assert_eq!(
    submitted.iter().map(|b| b.rows).sum::<usize>(),
    damage.height as usize,
    "行带行数之和应等于矩形高度"
  );
  assert_eq!(
    submitted[0],
    Band {
      x: 60,
      y: 100,
      width: 40,
      rows: 48
    }
  );
  assert_eq!(
    submitted[1],
    Band {
      x: 60,
      y: 148,
      width: 40,
      rows: 12
    },
    "末段的行号与余数不对"
  );
}

/// 越过右下角的矩形先裁到视口内，再按剩下的范围切分与拷贝。
#[test]
fn 越过视口右下角的矩形裁到视口内() {
  let frame = synthetic(VIEW);
  let plan: Vec<Band> = bands(
    Damage {
      x: 200,
      y: 270,
      width: 80,
      height: 40,
    },
    VIEW,
    BAND_ROWS,
  )
  .collect();
  assert_eq!(
    plan,
    vec![Band {
      x: 200,
      y: 270,
      width: 40,
      rows: 10
    }],
    "越界部分应裁掉"
  );

  let mut band = vec![0u32; VIEW.width * BAND_ROWS];
  for line in plan {
    let pixels = copy_band(&frame, &mut band, VIEW.width, line);
    assert_eq!(pixels, 40 * 10);
    assert_eq!(band[0], 270 * 1000 + 200, "裁出来的第一条取的像素不对");
    assert_eq!(band[40 * 9 + 39], 279 * 1000 + 239, "最后一行最后一列超出视口");
  }
}

/// 左上角越界的矩形同样裁到视口内：负坐标不参与取像素。
#[test]
fn 负坐标的矩形裁到视口内() {
  let frame = synthetic(VIEW);
  let plan: Vec<Band> = bands(
    Damage {
      x: -30,
      y: -10,
      width: 60,
      height: 30,
    },
    VIEW,
    BAND_ROWS,
  )
  .collect();
  assert_eq!(
    plan,
    vec![Band {
      x: 0,
      y: 0,
      width: 30,
      rows: 20
    }],
    "越出左上角的部分应裁掉"
  );

  let mut band = vec![0u32; VIEW.width * BAND_ROWS];
  let pixels = copy_band(&frame, &mut band, VIEW.width, plan[0]);
  assert_eq!(pixels, 30 * 20);
  assert_eq!(band[0], 0, "左上角第一像素应是整帧的原点");
  assert_eq!(band[30 * 19 + 29], 19 * 1000 + 29, "裁出来的最后一行取错了行");
}

/// 空的或完全在视口外的矩形不产出条目（也不碰行带缓冲）。
#[test]
fn 空的或完全在视口外的矩形不产生行带() {
  let cases = [
    Damage {
      x: 10,
      y: 10,
      width: 0,
      height: 40,
    },
    Damage {
      x: 10,
      y: 10,
      width: 40,
      height: 0,
    },
    Damage {
      x: 240,
      y: 0,
      width: 40,
      height: 40,
    },
    Damage {
      x: -50,
      y: 10,
      width: 50,
      height: 40,
    },
    Damage {
      x: 0,
      y: 280,
      width: 40,
      height: 40,
    },
  ];
  for case in cases {
    assert_eq!(bands(case, VIEW, BAND_ROWS).count(), 0, "不该产出条目：{case:?}");
  }
}

/// 行带高度传 0 时按一行切分：不产出条目会让这一帧永远提交不完。
#[test]
fn 行带高度为零时按一行切分() {
  let plan: Vec<Band> = bands(
    Damage {
      x: 0,
      y: 0,
      width: 240,
      height: 3,
    },
    VIEW,
    0,
  )
  .collect();
  assert_eq!(plan.len(), 3);
  assert!(plan.iter().all(|b| b.rows == 1));
}

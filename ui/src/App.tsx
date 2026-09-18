/**
 * Remapad 屏幕应用主界面：
 * - 上半部分：四叶草菜单区域 (x:0, y:0, w:240, h:200)，支持左右跟手滑动与平滑过渡动画，
 *   三槽位联动显示左、中、右相邻页面（滑动不露白），左右无限循环切换（阈值 80px），
 *   方向键/肩键左右翻页、上下选择；
 * - 下半部分：底部圆角状态卡片 (w:224, h:64)，集成物理手柄状态、主机连接（玩家指示灯/HOME触控）、
 *   电池电量，优先级支持 OTA 进度和手柄控屏按键提示；
 * - 弹窗打开时禁用左右切页，并支持按叉键（Cancel / Escape）退出。
 */
import { ref } from 'vue';
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';
import { attachGesture } from '@pocketjs/framework/vue-vapor/gesture';
import { animate, cancelAnim, jump } from '@pocketjs/framework/vue-vapor/animation';
import { onButtonPress, onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { BottomBar } from './components/BottomBar';
import { ConfirmDialog } from './components/ConfirmDialog';
import { useHardware, hw, powerOffDevice, rebootDevice } from './hooks/useHardware';
import { usePadControl } from './hooks/usePadControl';
import { BrightnessPage } from './pages/BrightnessPage';
import { ControllerSettingsPage } from './pages/ControllerSettingsPage';
import { PairingPage } from './pages/PairingPage';
import { PowerPage } from './pages/PowerPage';
import { SystemInfoPage } from './pages/SystemInfoPage';
import { DebugPage } from './pages/DebugPage';
import { IS_DEV } from './env.generated';
import { CHARSET_ANCHOR, COLOR, STYLE } from './theme';
import { ticksForMs } from './tick';

// 构建期字符集锚点：确保动态字符被扫描烘焙进字体图集
void CHARSET_ANCHOR;

const SWIPE_THRESHOLD = 40;
const SLOT_DISTANCE = 200;
const ANIM_DUR_MS = 120;
const REBOUND_DUR_MS = 100;
const BTN_CROSS = 0x4000;

export default function App() {
  useHardware();

  // 当前页索引（默认开机为第 0 页：亮度调节）
  const pageIndex = ref(0);
  const rebootAsk = ref(false);
  const powerOffAsk = ref(false);

  // 页面总数（若开启 dev 编译选项则包含第 6 页调试页）
  const pageCount = () => (IS_DEV ? 6 : 5);

  const prevPageIndex = () => {
    const total = pageCount();
    return (pageIndex.value - 1 + total) % total;
  };

  const nextPageIndex = () => {
    const total = pageCount();
    return (pageIndex.value + 1) % total;
  };

  const dialogOpen = () => rebootAsk.value || powerOffAsk.value;
  const interactive = (index: number) => () => pageIndex.value === index && !dialogOpen();

  // 四叶草动画与跟手位移控制器
  let carouselNode: NodeMirror | null = null;
  let currentAnimId = -1;
  let isAnimating = false;
  let pendingSlideFrames = 0;
  let pendingSlideTarget = -1;
  let pendingReboundFrames = 0;

  const setCarouselRef = (node: NodeMirror | null) => {
    carouselNode = node;
  };

  const clearTransition = () => {
    if (currentAnimId >= 0) {
      cancelAnim(currentAnimId);
      currentAnimId = -1;
    }
    pendingSlideFrames = 0;
    pendingSlideTarget = -1;
    pendingReboundFrames = 0;
    isAnimating = false;
  };

  // 动画到达终点后原子归位：位移复位至 0、切换目标页码，杜绝跳帧与二次闪烁
  const finishSlide = (targetPageIndex: number) => {
    clearTransition();
    if (carouselNode) {
      jump(carouselNode, 'translateX', 0);
    }
    pageIndex.value = targetPageIndex;
  };

  const finishRebound = () => {
    clearTransition();
    if (carouselNode) {
      jump(carouselNode, 'translateX', 0);
    }
  };

  onFrame(() => {
    if (pendingSlideFrames > 0) {
      pendingSlideFrames -= 1;
      if (pendingSlideFrames <= 0) {
        finishSlide(pendingSlideTarget);
      }
    } else if (pendingReboundFrames > 0) {
      pendingReboundFrames -= 1;
      if (pendingReboundFrames <= 0) {
        finishRebound();
      }
    }
  });

  const slideToNext = (fromX = 0) => {
    if (!carouselNode) {
      pageIndex.value = nextPageIndex();
      return;
    }
    clearTransition();
    isAnimating = true;
    jump(carouselNode, 'translateX', fromX);
    currentAnimId = animate(carouselNode, 'translateX', -SLOT_DISTANCE, {
      dur: ANIM_DUR_MS,
      easing: 'out',
    });
    pendingSlideTarget = nextPageIndex();
    pendingSlideFrames = Math.max(1, Math.round(ticksForMs(ANIM_DUR_MS)));
  };

  const slideToPrev = (fromX = 0) => {
    if (!carouselNode) {
      pageIndex.value = prevPageIndex();
      return;
    }
    clearTransition();
    isAnimating = true;
    jump(carouselNode, 'translateX', fromX);
    currentAnimId = animate(carouselNode, 'translateX', SLOT_DISTANCE, {
      dur: ANIM_DUR_MS,
      easing: 'out',
    });
    pendingSlideTarget = prevPageIndex();
    pendingSlideFrames = Math.max(1, Math.round(ticksForMs(ANIM_DUR_MS)));
  };

  const rebound = (fromX: number) => {
    if (!carouselNode) {
      return;
    }
    clearTransition();
    isAnimating = true;
    jump(carouselNode, 'translateX', fromX);
    currentAnimId = animate(carouselNode, 'translateX', 0, {
      dur: REBOUND_DUR_MS,
      easing: 'out',
    });
    pendingReboundFrames = Math.max(1, Math.round(ticksForMs(REBOUND_DUR_MS)));
  };

  const nextPage = (withAnim = true) => {
    if (dialogOpen() || isAnimating) {
      return;
    }
    if (!withAnim || !carouselNode) {
      pageIndex.value = nextPageIndex();
      return;
    }
    slideToNext(0);
  };

  const prevPage = (withAnim = true) => {
    if (dialogOpen() || isAnimating) {
      return;
    }
    if (!withAnim || !carouselNode) {
      pageIndex.value = prevPageIndex();
      return;
    }
    slideToPrev(0);
  };

  // 手势接管上半区域 (240 × 200)，支持实时跟手与阈值翻页（弹窗时阻断手势）
  attachGesture({
    axis: 'x',
    region: { rect: () => (dialogOpen() ? null : { x: 0, y: 0, w: 240, h: 200 }) },
    onPanStart: () => {
      clearTransition();
    },
    onPanMove: (contact) => {
      if (carouselNode) {
        jump(carouselNode, 'translateX', contact.dx);
      }
    },
    onPanEnd: (contact) => {
      if (contact.dx < -SWIPE_THRESHOLD) {
        slideToNext(contact.dx);
      } else if (contact.dx > SWIPE_THRESHOLD) {
        slideToPrev(contact.dx);
      } else {
        rebound(contact.dx);
      }
    },
  });

  // 按叉键（CROSS / Escape）关闭打开的确认对话框
  onButtonPress(BTN_CROSS, () => {
    if (dialogOpen()) {
      rebootAsk.value = false;
      powerOffAsk.value = false;
    }
  });

  // 手柄操控按键路由
  let pageRoot: NodeMirror | null = null;
  const padPageRef = (node: NodeMirror | null) => {
    pageRoot = node;
  };

  usePadControl({
    firmwareMode: () => hw.padUiMode,
    pageRoot: () => pageRoot,
    onPrevPage: () => prevPage(true),
    onNextPage: () => nextPage(true),
  });

  const confirmReboot = () => {
    rebootAsk.value = false;
    rebootDevice();
  };

  const confirmPowerOff = () => {
    powerOffAsk.value = false;
    powerOffDevice();
  };

  return (
    <View class={STYLE.appRoot}>
      {/* 上半部分：四叶草菜单区域 (y: 0 ~ 200) */}
      <View class="absolute top-0 left-0 w-full h-[200] overflow-hidden">
        <View
          nodeRef={setCarouselRef}
          class="w-full h-full relative"
        >
          {/* 中间主四叶草外框背景 (几何中心 x: 120, y: 104) */}
          <Image
            src="main.svg"
            class="absolute left-[-8] top-[-24] w-[256] h-[256]"
          />

          {/* 远左侧相邻四叶草背景 (几何中心 x: -280, y: 104，滑动向右时无缝衔接) */}
          <Image
            src="main.svg"
            class="absolute left-[-408] top-[-24] w-[256] h-[256]"
          />

          {/* 左侧相邻四叶草背景 (几何中心 x: -80, y: 104) */}
          <Image
            src="main.svg"
            class="absolute left-[-208] top-[-24] w-[256] h-[256]"
          />

          {/* 右侧相邻四叶草背景 (几何中心 x: 320, y: 104) */}
          <Image
            src="main.svg"
            class="absolute left-[192] top-[-24] w-[256] h-[256]"
          />

          {/* 远右侧相邻四叶草背景 (几何中心 x: 520, y: 104，滑动向左时无缝衔接) */}
          <Image
            src="main.svg"
            class="absolute left-[392] top-[-24] w-[256] h-[256]"
          />

          {/* 中间主槽位内容区：当前激活页（优先挂载、优先交互、优先焦点命中） */}
          <View
            nodeRef={padPageRef}
            class="absolute left-[42] top-[30] w-[156] h-[148] flex-col items-center justify-center overflow-hidden"
          >
            <BrightnessPage active={() => pageIndex.value === 0} interactive={interactive(0)} />
            <ControllerSettingsPage active={() => pageIndex.value === 1} interactive={interactive(1)} />
            <PairingPage active={() => pageIndex.value === 2} interactive={interactive(2)} />
            <PowerPage
              active={() => pageIndex.value === 3}
              interactive={interactive(3)}
              onAskReboot={() => { rebootAsk.value = true; }}
              onAskPowerOff={() => { powerOffAsk.value = true; }}
            />
            <SystemInfoPage active={() => pageIndex.value === 4} interactive={interactive(4)} />
            {IS_DEV ? <DebugPage active={() => pageIndex.value === 5} interactive={interactive(5)} /> : null}
          </View>

          {/* 左侧槽位内容区：实时呈现前一页，滑动时不留白 (几何中心 x: -80, y: 104, 尺寸 156 × 148) */}
          <View class='absolute left-[-158] top-[30] w-[156] h-[148] flex-col items-center justify-center overflow-hidden'>
            <BrightnessPage active={() => prevPageIndex() === 0} interactive={() => false} />
            <ControllerSettingsPage active={() => prevPageIndex() === 1} interactive={() => false} />
            <PairingPage active={() => prevPageIndex() === 2} interactive={() => false} />
            <PowerPage
              active={() => prevPageIndex() === 3}
              interactive={() => false}
              onAskReboot={() => { rebootAsk.value = true; }}
              onAskPowerOff={() => { powerOffAsk.value = true; }}
            />
            <SystemInfoPage active={() => prevPageIndex() === 4} interactive={() => false} />
            {IS_DEV ? <DebugPage active={() => prevPageIndex() === 5} interactive={() => false} /> : null}
          </View>

          {/* 右侧槽位内容区：实时呈现后一页，滑动时不留白 (几何中心 x: 320, y: 104, 尺寸 156 × 148) */}
          <View class='absolute left-[242] top-[30] w-[156] h-[148] flex-col items-center justify-center overflow-hidden'>
            <BrightnessPage active={() => nextPageIndex() === 0} interactive={() => false} />
            <ControllerSettingsPage active={() => nextPageIndex() === 1} interactive={() => false} />
            <PairingPage active={() => nextPageIndex() === 2} interactive={() => false} />
            <PowerPage
              active={() => nextPageIndex() === 3}
              interactive={() => false}
              onAskReboot={() => { rebootAsk.value = true; }}
              onAskPowerOff={() => { powerOffAsk.value = true; }}
            />
            <SystemInfoPage active={() => nextPageIndex() === 4} interactive={() => false} />
            {IS_DEV ? <DebugPage active={() => nextPageIndex() === 5} interactive={() => false} /> : null}
          </View>
        </View>
      </View>

      {/* 下半部分：底部三态圆角状态栏 (y: 208 ~ 272) */}
      <BottomBar />

      {/* 重启确认弹窗 */}
      {rebootAsk.value ? (
        <ConfirmDialog
          title="重启设备"
          lines={['是否立即重启 Remapad 设备？', '未保存的即时调试状态将丢失。']}
          confirmLabel="重启"
          onCancel={() => { rebootAsk.value = false; }}
          onConfirm={confirmReboot}
        />
      ) : null}

      {/* 关机确认弹窗 */}
      {powerOffAsk.value ? (
        <ConfirmDialog
          title="设备关机"
          lines={['是否立即关闭 Remapad 设备？', 'USB 供电时电源锁存仍将保持。']}
          confirmLabel="关机"
          onCancel={() => { powerOffAsk.value = false; }}
          onConfirm={confirmPowerOff}
        />
      ) : null}

      {/* 关机等待全屏遮罩 */}
      {hw.poweringOff ? (
        <View class={STYLE.busyOverlay}>
          <Text class="text-sm font-bold" style={{ textColor: COLOR.onSurface }}>
            关机中…
          </Text>
        </View>
      ) : null}

      {/* 重启中全屏遮罩 */}
      {hw.rebooting ? (
        <View class={STYLE.busyOverlay}>
          <Text class="text-sm font-bold" style={{ textColor: COLOR.onSurface }}>
            重启中…
          </Text>
        </View>
      ) : null}
    </View>
  );
}

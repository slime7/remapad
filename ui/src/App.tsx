/**
 * Remapad 屏幕应用主界面：
 * - 上半部分：四叶草菜单区域 (x:0, y:0, w:240, h:200)，单卡片居中展示，取消边缘露出提示以消除横向撕裂；
 *   切页时旧卡片原地淡出缩放至 95%，新卡片从 95% 尺寸一边放大至 100% 一边淡入；
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

const SWIPE_THRESHOLD = 50;
const SWIPE_SPAN = 100;
const ANIM_DUR_MS = 160;
const REBOUND_DUR_MS = 100;
const BTN_CROSS = 0x4000;

export default function App() {
  useHardware();

  // 当前页索引（默认开机为第 0 页：亮度调节）
  const pageIndex = ref(0);
  // 过渡中目标页（-1 为静止态无过渡）
  const transitionTarget = ref(-1);
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
  const isCardVisible = (index: number) => pageIndex.value === index || transitionTarget.value === index;
  const isPageActive = (index: number) => pageIndex.value === index || transitionTarget.value === index;
  const interactive = (index: number) => () => pageIndex.value === index && !dialogOpen() && !isAnimating;

  // 卡片节点与交叉缩放淡入淡出控制器
  const cardNodes: (NodeMirror | null)[] = [null, null, null, null, null, null];
  let fromIndex = -1;
  let isAnimating = false;
  let isDragging = false;
  let animIds: number[] = [];
  let pendingAnimFrames = 0;
  let pendingReboundFrames = 0;

  const setCardRef = (index: number) => (node: NodeMirror | null) => {
    cardNodes[index] = node;
  };

  const clearTransition = () => {
    for (const id of animIds) {
      if (id >= 0) {
        cancelAnim(id);
      }
    }
    animIds = [];
    pendingAnimFrames = 0;
    pendingReboundFrames = 0;
    isAnimating = false;
    isDragging = false;
  };

  // 动画终点结算：原子切换活跃页码并复位卡片变换状态
  const finishTransition = () => {
    const target = transitionTarget.value;
    const from = fromIndex;
    clearTransition();
    if (target >= 0) {
      pageIndex.value = target;
    }
    transitionTarget.value = -1;
    fromIndex = -1;
    const oldNode = from >= 0 ? cardNodes[from] : null;
    const newNode = target >= 0 ? cardNodes[target] : null;
    if (oldNode) {
      jump(oldNode, 'opacity', 1);
      jump(oldNode, 'scale', 1.0);
    }
    if (newNode) {
      jump(newNode, 'opacity', 1);
      jump(newNode, 'scale', 1.0);
    }
  };

  const finishRebound = () => {
    const from = fromIndex;
    const target = transitionTarget.value;
    clearTransition();
    transitionTarget.value = -1;
    fromIndex = -1;
    const oldNode = from >= 0 ? cardNodes[from] : null;
    const newNode = target >= 0 ? cardNodes[target] : null;
    if (oldNode) {
      jump(oldNode, 'opacity', 1);
      jump(oldNode, 'scale', 1.0);
    }
    if (newNode) {
      jump(newNode, 'opacity', 0);
      jump(newNode, 'scale', 0.95);
    }
  };

  onFrame(() => {
    if (pendingAnimFrames > 0) {
      pendingAnimFrames -= 1;
      if (pendingAnimFrames <= 0) {
        finishTransition();
      }
    } else if (pendingReboundFrames > 0) {
      pendingReboundFrames -= 1;
      if (pendingReboundFrames <= 0) {
        finishRebound();
      }
    }
  });

  const startTransition = (to: number) => {
    if (dialogOpen() || isAnimating) {
      return;
    }
    const from = pageIndex.value;
    if (from === to) {
      return;
    }

    const oldNode = cardNodes[from];
    const newNode = cardNodes[to];
    if (!oldNode || !newNode) {
      pageIndex.value = to;
      transitionTarget.value = -1;
      return;
    }

    clearTransition();
    isAnimating = true;
    fromIndex = from;
    transitionTarget.value = to;

    // 旧卡片以 1.0 满尺寸与不透明起始
    jump(oldNode, 'opacity', 1);
    jump(oldNode, 'scale', 1.0);

    // 新卡片以 0.95 尺寸与完全透明起始
    jump(newNode, 'opacity', 0);
    jump(newNode, 'scale', 0.95);

    // 旧卡片在原地淡出并微缩至 95%，新卡片从 95% 放大至 100% 并淡入
    animIds = [
      animate(oldNode, 'opacity', 0, { dur: ANIM_DUR_MS, easing: 'out' }),
      animate(oldNode, 'scale', 0.95, { dur: ANIM_DUR_MS, easing: 'out' }),
      animate(newNode, 'opacity', 1, { dur: ANIM_DUR_MS, easing: 'out' }),
      animate(newNode, 'scale', 1.0, { dur: ANIM_DUR_MS, easing: 'out' }),
    ];
    pendingAnimFrames = Math.max(1, Math.round(ticksForMs(ANIM_DUR_MS)));
  };

  const nextPage = () => {
    startTransition(nextPageIndex());
  };

  const prevPage = () => {
    startTransition(prevPageIndex());
  };

  // 手势接管上半区域 (240 × 200)，支持拖动实时微缩淡出跟手与平滑回弹/顺应切页
  attachGesture({
    axis: 'x',
    region: { rect: () => (dialogOpen() ? null : { x: 0, y: 0, w: 240, h: 200 }) },
    onPanStart: () => {
      if (dialogOpen() || isAnimating) {
        return;
      }
      clearTransition();
      fromIndex = pageIndex.value;
      isDragging = true;
    },
    onPanMove: (contact) => {
      if (!isDragging || dialogOpen()) {
        return;
      }
      const target = contact.dx < 0 ? nextPageIndex() : prevPageIndex();
      if (transitionTarget.value !== target) {
        transitionTarget.value = target;
      }
      const oldNode = cardNodes[pageIndex.value];
      const newNode = cardNodes[target];
      const progress = Math.min(1.0, Math.abs(contact.dx) / SWIPE_SPAN);
      if (oldNode) {
        jump(oldNode, 'scale', 1.0 - 0.05 * progress);
        jump(oldNode, 'opacity', 1.0 - progress);
      }
      if (newNode) {
        jump(newNode, 'scale', 0.95 + 0.05 * progress);
        jump(newNode, 'opacity', progress);
      }
    },
    onPanEnd: (contact) => {
      if (!isDragging) {
        return;
      }
      isDragging = false;
      const target = transitionTarget.value;
      const oldNode = cardNodes[pageIndex.value];
      const newNode = target >= 0 ? cardNodes[target] : null;

      if (Math.abs(contact.dx) >= SWIPE_THRESHOLD && target >= 0 && oldNode && newNode) {
        // 超过阈值：顺应当前进度平滑过渡到终点
        isAnimating = true;
        const progress = Math.min(1.0, Math.abs(contact.dx) / SWIPE_SPAN);
        const remMs = Math.max(80, Math.round(ANIM_DUR_MS * (1 - progress * 0.4)));
        animIds = [
          animate(oldNode, 'scale', 0.95, { dur: remMs, easing: 'out' }),
          animate(oldNode, 'opacity', 0, { dur: remMs, easing: 'out' }),
          animate(newNode, 'scale', 1.0, { dur: remMs, easing: 'out' }),
          animate(newNode, 'opacity', 1, { dur: remMs, easing: 'out' }),
        ];
        pendingAnimFrames = Math.max(1, Math.round(ticksForMs(remMs)));
      } else if (target >= 0 && oldNode && newNode) {
        // 未超阈值：平滑回弹复原
        isAnimating = true;
        animIds = [
          animate(oldNode, 'scale', 1.0, { dur: REBOUND_DUR_MS, easing: 'out' }),
          animate(oldNode, 'opacity', 1, { dur: REBOUND_DUR_MS, easing: 'out' }),
          animate(newNode, 'scale', 0.95, { dur: REBOUND_DUR_MS, easing: 'out' }),
          animate(newNode, 'opacity', 0, { dur: REBOUND_DUR_MS, easing: 'out' }),
        ];
        pendingReboundFrames = Math.max(1, Math.round(ticksForMs(REBOUND_DUR_MS)));
      } else {
        clearTransition();
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
    onPrevPage: () => prevPage(),
    onNextPage: () => nextPage(),
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
      {/* 上半部分：四叶草单卡片区域 (y: 0 ~ 200)，取消边缘露出 */}
      <View
        nodeRef={padPageRef}
        class="absolute top-0 left-0 w-full h-[200] overflow-hidden"
      >
        {/* 第 1 页：亮度调节 (几何中心 x: 120, y: 104) */}
        <View
          nodeRef={setCardRef(0)}
          class={isCardVisible(0) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden">
            <BrightnessPage active={() => isPageActive(0)} interactive={interactive(0)} />
          </View>
        </View>

        {/* 第 2 页：手柄设置 (几何中心 x: 120, y: 104) */}
        <View
          nodeRef={setCardRef(1)}
          class={isCardVisible(1) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden">
            <ControllerSettingsPage active={() => isPageActive(1)} interactive={interactive(1)} />
          </View>
        </View>

        {/* 第 3 页：手柄配对 (几何中心 x: 120, y: 104) */}
        <View
          nodeRef={setCardRef(2)}
          class={isCardVisible(2) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden">
            <PairingPage active={() => isPageActive(2)} interactive={interactive(2)} />
          </View>
        </View>

        {/* 第 4 页：电源管理 (几何中心 x: 120, y: 104) */}
        <View
          nodeRef={setCardRef(3)}
          class={isCardVisible(3) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden">
            <PowerPage
              active={() => isPageActive(3)}
              interactive={interactive(3)}
              onAskReboot={() => { rebootAsk.value = true; }}
              onAskPowerOff={() => { powerOffAsk.value = true; }}
            />
          </View>
        </View>

        {/* 第 5 页：系统信息 (几何中心 x: 120, y: 104) */}
        <View
          nodeRef={setCardRef(4)}
          class={isCardVisible(4) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden">
            <SystemInfoPage active={() => isPageActive(4)} interactive={interactive(4)} />
          </View>
        </View>

        {/* 第 6 页：调试指令 (开发模式) */}
        {IS_DEV ? (
          <View
            nodeRef={setCardRef(5)}
            class={isCardVisible(5) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
          >
            <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
            <View class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden">
              <DebugPage active={() => isPageActive(5)} interactive={interactive(5)} />
            </View>
          </View>
        ) : null}
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

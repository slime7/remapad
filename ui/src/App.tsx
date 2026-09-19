/**
 * Remapad 屏幕应用主界面：
 * - 上半部分：四叶草菜单区域 (x:0, y:0, w:240, h:200)，单卡片居中展示，取消边缘露出提示以消除横向撕裂；
 *   切页过渡不做透明度混合（S3 软渲染的逐像素混合开销大）：旧卡立即让位，
 *   新卡从 75% 尺寸单独放大至 100%，拖动预览同比例缩小当前卡；
 *   方向键/肩键左右翻页、上下选择；拖动越过阈值或快甩释放速度达标即翻页，
 *   左右凹陷处的翻页箭头点按切页（触摸专用，不进手柄焦点环）；
 * - 下半部分：底部圆角状态卡片 (w:224, h:64)，集成物理手柄状态、主机连接（玩家指示灯/HOME触控）、
 *   电池电量，优先级支持 OTA 进度和手柄控屏按键提示；
 * - 弹窗打开时禁用左右切页，并支持按叉键（Cancel / Escape）退出。
 */
import { ref } from 'vue';
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';
import { attachGesture } from '@pocketjs/framework/vue-vapor/gesture';
import { animate, cancelAnim, jump } from '@pocketjs/framework/vue-vapor/animation';
import { onButtonPress, onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { BottomBar } from './components/BottomBar';
import { ConfirmDialog } from './components/ConfirmDialog';
import { ICON, Icon } from './icons';
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
const SWIPE_SPAN = 100;
// 甩动提交：释放速度（逻辑 px/虚拟秒，框架按 60Hz 采样）达到阈值、且总位移
// 越过抖动下限（高于 tapSlop）即切页——快划时手指早抬、总位移很小也能翻页。
const FLING_VELOCITY = 500;
const FLING_MIN_TRAVEL = 12;
// 过渡缩放下限：新卡从 75% 放大进场，拖动预览把当前卡按进度缩到 75%。
// 过渡全程不做透明度混合——S3 软件渲染器的逐像素混合是过渡期最大的单帧开销。
const TRANSITION_SCALE = 0.75;
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
  // 过渡可见性：提交动画期间只显示目标卡（旧卡立即让位，新卡放大进场），
  // 拖动预览与静止态只显示当前页卡。两个引用都无条件读取——条件短路会让
  // 其中一个漏出类绑定的依赖清单，写入就不再触发可见性翻转。
  const isCardVisible = (index: number) => {
    const page = pageIndex.value;
    const target = transitionTarget.value;
    if (isAnimating.value && target >= 0) {
      return target === index;
    }
    return page === index;
  };
  const isPageActive = (index: number) => isCardVisible(index);
  const interactive = (index: number) => () =>
    pageIndex.value === index && !dialogOpen() && !isAnimating.value;

  // 卡片节点与过渡控制器。isAnimating 必须是响应式引用：提交路径（onPanEnd）
  // 里 transitionTarget 不变、只有它翻转，普通变量对类绑定不可见，新卡会在
  // 整个放大动画期间保持隐藏，动画跑完才随 pageIndex 瞬间换页。
  const cardNodes: (NodeMirror | null)[] = [null, null, null, null, null, null];
  let fromIndex = -1;
  const isAnimating = ref(false);
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
    isAnimating.value = false;
    isDragging = false;
    transitionTarget.value = -1;
    fromIndex = -1;
  };

  // 动画终点结算：原子切换活跃页码并复位卡片变换状态
  const finishTransition = () => {
    const target = transitionTarget.value;
    const from = fromIndex;
    clearTransition();
    if (target >= 0) {
      pageIndex.value = target;
    }
    const oldNode = from >= 0 ? cardNodes[from] : null;
    const newNode = target >= 0 ? cardNodes[target] : null;
    if (oldNode) {
      jump(oldNode, 'scale', 1.0);
    }
    if (newNode) {
      jump(newNode, 'scale', 1.0);
    }
  };

  const finishRebound = () => {
    const from = fromIndex;
    clearTransition();
    const oldNode = from >= 0 ? cardNodes[from] : null;
    if (oldNode) {
      jump(oldNode, 'scale', 1.0);
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
    if (dialogOpen() || isAnimating.value) {
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
    isAnimating.value = true;
    fromIndex = from;
    transitionTarget.value = to;

    // 提交过渡：旧卡经可见性立即让位，新卡从 75% 单独放大进场（无透明度混合）
    jump(newNode, 'scale', TRANSITION_SCALE);
    animIds = [
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
      if (dialogOpen() || isAnimating.value) {
        return;
      }
      clearTransition();
      fromIndex = pageIndex.value;
      isDragging = true;
      const oldNode = cardNodes[fromIndex];
      if (oldNode) {
        jump(oldNode, 'scale', 1.0);
      }
    },
    onPanMove: (contact) => {
      if (!isDragging || dialogOpen()) {
        return;
      }
      const target = contact.dx < 0 ? nextPageIndex() : prevPageIndex();
      if (transitionTarget.value !== target) {
        transitionTarget.value = target;
      }
      // 拖动预览只缩放当前卡（新卡保持隐藏，透明度不参与）：
      // 进度 0→1 对应 100%→75%
      const oldNode = cardNodes[pageIndex.value];
      const progress = Math.min(1.0, Math.abs(contact.dx) / SWIPE_SPAN);
      if (oldNode) {
        jump(oldNode, 'scale', 1.0 - (1.0 - TRANSITION_SCALE) * progress);
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

      const flick = Math.abs(contact.vx) >= FLING_VELOCITY && Math.abs(contact.dx) >= FLING_MIN_TRAVEL;
      if ((Math.abs(contact.dx) >= SWIPE_THRESHOLD || flick) && target >= 0 && oldNode && newNode) {
        // 顺应提交：旧卡让位，新卡从 75% 放大进场（不做透明度混合）
        isAnimating.value = true;
        jump(newNode, 'scale', TRANSITION_SCALE);
        animIds = [
          animate(newNode, 'scale', 1.0, { dur: ANIM_DUR_MS, easing: 'out' }),
        ];
        pendingAnimFrames = Math.max(1, Math.round(ticksForMs(ANIM_DUR_MS)));
      } else if (target >= 0 && oldNode && newNode) {
        // 未超阈值：当前卡缩放回弹复原（新卡未上过屏，先收起过渡目标）
        isAnimating.value = true;
        transitionTarget.value = -1;
        animIds = [
          animate(oldNode, 'scale', 1.0, { dur: REBOUND_DUR_MS, easing: 'out' }),
        ];
        pendingReboundFrames = Math.max(1, Math.round(ticksForMs(REBOUND_DUR_MS)));
      } else {
        clearTransition();
      }
    },
  });

  // 四叶草左右凹陷处的翻页箭头：触摸点按切页。绑节点区域（透明触摸区命中
  // 走 op 42 边界判定），不设 focusable、也不在 pageRoot 子树内，方向键与
  // 圆圈键的焦点环因此永远落不到箭头上。
  let leftArrowNode: NodeMirror | null = null;
  const leftArrowRef = (node: NodeMirror | null) => {
    leftArrowNode = node;
  };
  let rightArrowNode: NodeMirror | null = null;
  const rightArrowRef = (node: NodeMirror | null) => {
    rightArrowNode = node;
  };

  attachGesture({
    region: { node: () => leftArrowNode },
    onTap: () => prevPage(),
  });
  attachGesture({
    region: { node: () => rightArrowNode },
    onTap: () => nextPage(),
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

      {/* 左右翻页箭头：落在四叶草左右凹陷的深色区（触摸点按切页，见上方手势注册） */}
      <View
        nodeRef={leftArrowRef}
        class="absolute left-[2] top-[88] w-[32] h-[32] flex-row items-center justify-center"
      >
        <Icon glyph={ICON.arrowBack} class="text-lg shrink-0" color={COLOR.onSurfaceVariant} />
      </View>
      <View
        nodeRef={rightArrowRef}
        class="absolute left-[206] top-[88] w-[32] h-[32] flex-row items-center justify-center"
      >
        <Icon glyph={ICON.arrowForward} class="text-lg shrink-0" color={COLOR.onSurfaceVariant} />
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

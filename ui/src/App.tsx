/**
 * Remapad 屏幕应用主界面：
 * - 上半部分：四叶草菜单区域 (x:0, y:0, w:240, h:200)，单卡片居中展示，取消边缘露出提示以消除横向撕裂；
 *   切页瞬时完成：任何搬动整页内容的动画每帧都要重画内容框，在 S3 上比一次
 *   瞬时切换贵好几倍，方向提示改由行进侧的翻页箭头弹一下承担（见 docs/adr/0050）；
 *   拖动预览仍让当前卡内容跟手平移（手指驱动，跟手反馈不可省）；
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
import { DsSettingsPage } from './pages/DsSettingsPage';
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
// 甩动提交：释放速度（逻辑 px/虚拟秒，框架按 60Hz 采样）达到阈值、且总位移
// 越过抖动下限（高于 tapSlop）即切页——快划时手指早抬、总位移很小也能翻页。
const FLING_VELOCITY = 500;
const FLING_MIN_TRAVEL = 12;
// 拖动预览：当前卡内容按手指位移的一半跟手平移，位移封顶 DRAG_SLIDE_PX。
const DRAG_SLIDE_PX = 24;
const DRAG_FOLLOW = 0.5;
// 切页提示：页面瞬时切换，只有方向侧的翻页箭头往外弹 ARROW_NUDGE_PX 再收回。
// 整页平移每帧都要重画内容框（S3 上一帧四十到八十毫秒，三帧动画比瞬时切换贵三倍），
// 箭头只有 32 × 32，弹一下的代价在毫秒级（见 docs/adr/0050）。
const ARROW_NUDGE_PX = 6;
const ARROW_NUDGE_MS = 90;
// 拖动未越过阈值时的回弹复原时长。
const REBOUND_DUR_MS = 60;
const BTN_CROSS = 0x4000;

export default function App() {
  useHardware();

  // 当前页索引（默认开机为第 0 页：亮度调节）
  const pageIndex = ref(0);
  // 过渡中目标页（-1 为静止态无过渡）
  const transitionTarget = ref(-1);
  const rebootAsk = ref(false);
  const powerOffAsk = ref(false);

  // 页面总数（若开启 dev 编译选项则包含第 7 页调试页）
  const pageCount = () => (IS_DEV ? 7 : 6);

  const prevPageIndex = () => {
    const total = pageCount();
    return (pageIndex.value - 1 + total) % total;
  };

  const nextPageIndex = () => {
    const total = pageCount();
    return (pageIndex.value + 1) % total;
  };

  const dialogOpen = () => rebootAsk.value || powerOffAsk.value;
  // 卡片可见性只看当前页码：切页是瞬时的，拖动预览期间显示的也还是当前页。
  const isCardVisible = (index: number) => pageIndex.value === index;
  const isPageActive = (index: number) => isCardVisible(index);
  const interactive = (index: number) => () =>
    pageIndex.value === index && !dialogOpen() && !isAnimating.value;

  // 页面内容节点与拖动控制器：切页瞬时完成，只有拖动回弹与方向提示有动画，
  // 内容节点只在拖动预览里被平移（translateX）。isAnimating 必须是响应式引用，
  // 类绑定才看得见它的翻转（回弹期间焦点环要收起来）。
  const contentNodes: (NodeMirror | null)[] = [null, null, null, null, null, null, null];
  let fromIndex = -1;
  const isAnimating = ref(false);
  let isDragging = false;
  let reboundIds: number[] = [];
  let cueIds: number[] = [];
  let pendingReboundFrames = 0;

  const setContentRef = (index: number) => (node: NodeMirror | null) => {
    contentNodes[index] = node;
  };

  /** 拖动状态归零：拖动预览与回弹都由它收尾。 */
  const clearDrag = () => {
    isDragging = false;
    transitionTarget.value = -1;
    fromIndex = -1;
  };

  /** 回弹收尾：内容位移归零、清掉动画句柄与拖动状态。 */
  const finishRebound = () => {
    const node = fromIndex >= 0 ? contentNodes[fromIndex] : null;
    for (const id of reboundIds) {
      if (id >= 0) {
        cancelAnim(id);
      }
    }
    reboundIds = [];
    pendingReboundFrames = 0;
    isAnimating.value = false;
    clearDrag();
    if (node) {
      jump(node, 'translateX', 0);
    }
  };

  onFrame(() => {
    if (pendingReboundFrames > 0) {
      pendingReboundFrames -= 1;
      if (pendingReboundFrames <= 0) {
        finishRebound();
      }
    }
  });

  // 四叶草左右凹陷处的翻页箭头：兼作方向提示与触摸切页区。绑节点区域（透明
  // 触摸区命中走 op 42 边界判定），不设 focusable、也不在 pageRoot 子树内，
  // 方向键与圆圈键的焦点环因此永远落不到箭头上。
  let leftArrowNode: NodeMirror | null = null;
  const leftArrowRef = (node: NodeMirror | null) => {
    leftArrowNode = node;
  };
  let rightArrowNode: NodeMirror | null = null;
  const rightArrowRef = (node: NodeMirror | null) => {
    rightArrowNode = node;
  };

  /** 方向提示：页面已经切过去了，只让行进侧的翻页箭头往外弹一下再收回。
   *  direction = +1 向下一页（右箭头动），-1 向上一页（左箭头动）。 */
  const nudgeArrow = (direction: number) => {
    for (const id of cueIds) {
      if (id >= 0) {
        cancelAnim(id);
      }
    }
    cueIds = [];
    const node = direction > 0 ? rightArrowNode : leftArrowNode;
    if (!node) {
      return;
    }
    jump(node, 'translateX', direction * ARROW_NUDGE_PX);
    cueIds.push(animate(node, 'translateX', 0, { dur: ARROW_NUDGE_MS, easing: 'out' }));
  };

  /** 立即切页：复位被拖动过的旧页内容、翻到目标页，再给一次方向提示。
   *  页面本身不做进场动画——整页平移每帧都要重画内容框，比瞬时切换贵好几倍。 */
  const switchPage = (to: number, direction: number) => {
    const oldNode = fromIndex >= 0 ? contentNodes[fromIndex] : null;
    for (const id of reboundIds) {
      if (id >= 0) {
        cancelAnim(id);
      }
    }
    reboundIds = [];
    pendingReboundFrames = 0;
    isAnimating.value = false;
    clearDrag();
    if (oldNode) {
      jump(oldNode, 'translateX', 0);
    }
    pageIndex.value = to;
    nudgeArrow(direction);
  };

  /** direction = +1 表示向下一页，-1 表示向上一页。 */
  const startTransition = (to: number, direction: number) => {
    if (dialogOpen() || isDragging || isAnimating.value || to === pageIndex.value) {
      return;
    }
    switchPage(to, direction);
  };

  const nextPage = () => {
    startTransition(nextPageIndex(), 1);
  };

  const prevPage = () => {
    startTransition(prevPageIndex(), -1);
  };

  // 手势接管上半区域 (240 × 200)，支持拖动实时跟手平移与平滑回弹/顺应切页
  attachGesture({
    axis: 'x',
    region: { rect: () => (dialogOpen() ? null : { x: 0, y: 0, w: 240, h: 200 }) },
    onPanStart: () => {
      if (dialogOpen() || isAnimating.value) {
        return;
      }
      fromIndex = pageIndex.value;
      isDragging = true;
      transitionTarget.value = -1;
      const oldNode = contentNodes[fromIndex];
      if (oldNode) {
        jump(oldNode, 'translateX', 0);
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
      // 拖动预览只平移当前卡内容（新卡保持隐藏，透明度不参与）：
      // 位移按 DRAG_FOLLOW 跟手，并对位移封顶，免得整段内容滑出框外
      const oldNode = contentNodes[pageIndex.value];
      if (oldNode) {
        const offset = Math.max(
          -DRAG_SLIDE_PX,
          Math.min(DRAG_SLIDE_PX, contact.dx * DRAG_FOLLOW),
        );
        jump(oldNode, 'translateX', offset);
      }
    },
    onPanEnd: (contact) => {
      if (!isDragging) {
        return;
      }
      isDragging = false;
      const target = transitionTarget.value;
      const from = fromIndex;
      const oldNode = from >= 0 ? contentNodes[from] : null;

      const flick = Math.abs(contact.vx) >= FLING_VELOCITY && Math.abs(contact.dx) >= FLING_MIN_TRAVEL;
      if ((Math.abs(contact.dx) >= SWIPE_THRESHOLD || flick) && target >= 0) {
        // 顺应提交：内容复位后立即切页，方向提示由 switchPage 给出
        switchPage(target, contact.dx < 0 ? 1 : -1);
      } else if (target >= 0 && oldNode) {
        // 未超阈值：当前卡内容平移回弹复原（内容位移是拖动留下的，只在这里清）
        isAnimating.value = true;
        transitionTarget.value = -1;
        reboundIds = [animate(oldNode, 'translateX', 0, { dur: REBOUND_DUR_MS, easing: 'out' })];
        pendingReboundFrames = Math.max(1, Math.round(ticksForMs(REBOUND_DUR_MS)));
      } else {
        clearDrag();
      }
    },
  });

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
        {/* 第 1 页：亮度调节 (几何中心 x: 120, y: 104)。角钮页铺满整张卡片：
            角钮要与背景瓣外弧同心，圆心落在中央内容框 (50,54,156,148) 之外。 */}
        <View
          class={isCardVisible(0) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(0)} class="absolute left-0 top-0 w-[256] h-[256]">
            <BrightnessPage active={() => isPageActive(0)} interactive={interactive(0)} />
          </View>
        </View>

        {/* 第 2 页：手柄设置 (几何中心 x: 120, y: 104) */}
        <View
          class={isCardVisible(1) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View
            nodeRef={setContentRef(1)}
            class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden"
          >
            <ControllerSettingsPage active={() => isPageActive(1)} interactive={interactive(1)} />
          </View>
        </View>

        {/* 第 3 页：手柄配对 (几何中心 x: 120, y: 104)。角钮页，容器同第 1 页。 */}
        <View
          class={isCardVisible(2) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(2)} class="absolute left-0 top-0 w-[256] h-[256]">
            <PairingPage active={() => isPageActive(2)} interactive={interactive(2)} />
          </View>
        </View>

        {/* 第 4 页：电源管理 (几何中心 x: 120, y: 104)。角钮页，容器同第 1 页。 */}
        <View
          class={isCardVisible(3) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(3)} class="absolute left-0 top-0 w-[256] h-[256]">
            <PowerPage
              active={() => isPageActive(3)}
              interactive={interactive(3)}
              onAskReboot={() => { rebootAsk.value = true; }}
              onAskPowerOff={() => { powerOffAsk.value = true; }}
            />
          </View>
        </View>

        {/* 第 5 页：DS4、DS5 设置 (几何中心 x: 120, y: 104) */}
        <View
          class={isCardVisible(4) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View
            nodeRef={setContentRef(4)}
            class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden"
          >
            <DsSettingsPage active={() => isPageActive(4)} interactive={interactive(4)} />
          </View>
        </View>

        {/* 第 6 页：系统信息 (几何中心 x: 120, y: 104) */}
        <View
          class={isCardVisible(5) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
        >
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View
            nodeRef={setContentRef(5)}
            class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden"
          >
            <SystemInfoPage active={() => isPageActive(5)} interactive={interactive(5)} />
          </View>
        </View>

        {/* 第 7 页：调试指令 (开发模式) */}
        {IS_DEV ? (
          <View
            class={isCardVisible(6) ? 'absolute left-[-8] top-[-24] w-[256] h-[256]' : 'hidden'}
          >
            <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
            <View
              nodeRef={setContentRef(6)}
              class="absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden"
            >
              <DebugPage active={() => isPageActive(6)} interactive={interactive(6)} />
            </View>
          </View>
        ) : null}
      </View>

      {/* 左右翻页箭头：落在四叶草左右凹陷的深色区（无柄 chevron，触摸点按切页，见上方手势注册） */}
      <View
        nodeRef={leftArrowRef}
        class="absolute left-[2] top-[88] w-[32] h-[32] flex-row items-center justify-center"
      >
        <Icon glyph={ICON.chevronLeft} class="text-lg shrink-0" color={COLOR.onSurfaceVariant} />
      </View>
      <View
        nodeRef={rightArrowRef}
        class="absolute left-[206] top-[88] w-[32] h-[32] flex-row items-center justify-center"
      >
        <Icon glyph={ICON.chevronRight} class="text-lg shrink-0" color={COLOR.onSurfaceVariant} />
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

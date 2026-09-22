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
import { useHardware, hw, powerOffDevice, rebootDevice, setUsbRole } from './hooks/useHardware';
import { usePadControl } from './hooks/usePadControl';
import { BrightnessPage } from './pages/BrightnessPage';
import { ControllerSettingsPage } from './pages/ControllerSettingsPage';
import { DsSettingsPage } from './pages/DsSettingsPage';
import { ModePage } from './pages/ModePage';
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

/**
 * 四叶草轮播页表：顺序即左右翻页顺序，新增或删除页面只改这里——槽位由数组
 * 位置推出来，各页代码按页名取槽位，页面描述里不再写死序号（见 docs/adr/0041）。
 * 开发构建把调试页接在末尾。
 */
const PAGE_KEYS = [
  'brightness',
  'controller',
  'pairing',
  'power',
  'usbMode',
  'dsSettings',
  'systemInfo',
] as const;
type PageKey = (typeof PAGE_KEYS)[number] | 'debug';
/** 页名 → 槽位索引（本页在轮播里的位置）。 */
const SLOT = (() => {
  const keys: PageKey[] = [...PAGE_KEYS];
  if (IS_DEV) {
    keys.push('debug');
  }
  const map = new Map<PageKey, number>();
  keys.forEach((key, index) => map.set(key, index));
  return map;
})();
const SLOT_COUNT = SLOT.size;
/** 取页槽位：页面代码与页表之间唯一的联系。 */
const slot = (key: PageKey) => SLOT.get(key) ?? 0;

/** 页容器两种形态：整张 256 卡片（角钮页铺满，钮心与背景瓣外弧同心）
 *  与中央内容框（列表与表单页用，130 × 130 左右）。 */
const CARD_BOX = 'absolute left-[-8] top-[-24] w-[256] h-[256]';
const CENTER_BOX =
  'absolute left-[50] top-[54] w-[156] h-[148] flex-col items-center justify-center overflow-hidden';

export default function App() {
  useHardware();

  // 当前页槽位（默认开机为亮度调节页）
  const pageIndex = ref(slot('brightness'));
  // 过渡中目标页（-1 为静止态无过渡）
  const transitionTarget = ref(-1);
  const rebootAsk = ref(false);
  const powerOffAsk = ref(false);
  /** 切到「手柄」的确认弹窗（切过去之后 PC 串口消失）。 */
  const usbHostAsk = ref(false);

  // 页面总数：发布构建七个，dev 构建含调试页八个（见上面的页表）
  const pageCount = () => SLOT_COUNT;

  const prevPageIndex = () => {
    const total = pageCount();
    return (pageIndex.value - 1 + total) % total;
  };

  const nextPageIndex = () => {
    const total = pageCount();
    return (pageIndex.value + 1) % total;
  };

  const dialogOpen = () =>
    rebootAsk.value || powerOffAsk.value || usbHostAsk.value || hw.usbRebootAsk;
  // 卡片可见性只看当前页码：切页是瞬时的，拖动预览期间显示的也还是当前页。
  const isCardVisible = (index: number) => pageIndex.value === index;
  const isPageActive = (index: number) => isCardVisible(index);
  const interactive = (index: number) => () =>
    pageIndex.value === index && !dialogOpen() && !isAnimating.value;

  // 页面内容节点与拖动控制器：切页瞬时完成，只有拖动回弹与方向提示有动画，
  // 内容节点只在拖动预览里被平移（translateX）。isAnimating 必须是响应式引用，
  // 类绑定才看得见它的翻转（回弹期间焦点环要收起来）。
  const contentNodes: (NodeMirror | null)[] = [];
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

  const confirmUsbHost = () => {
    usbHostAsk.value = false;
    setUsbRole('host');
  };

  return (
    <View class={STYLE.appRoot}>
      {/* 上半部分：四叶草单卡片区域 (y: 0 ~ 200)，取消边缘露出 */}
      <View
        nodeRef={padPageRef}
        class="absolute top-0 left-0 w-full h-[200] overflow-hidden"
      >
        {/* 亮度调节。角钮页铺满整张卡片：角钮要与背景瓣外弧同心，
            圆心落在中央内容框 (50,54,156,148) 之外。 */}
        <View class={isCardVisible(slot('brightness')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('brightness'))} class="absolute left-0 top-0 w-[256] h-[256]">
            <BrightnessPage
              active={() => isPageActive(slot('brightness'))}
              interactive={interactive(slot('brightness'))}
            />
          </View>
        </View>

        {/* 手柄设置 */}
        <View class={isCardVisible(slot('controller')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('controller'))} class={CENTER_BOX}>
            <ControllerSettingsPage
              active={() => isPageActive(slot('controller'))}
              interactive={interactive(slot('controller'))}
            />
          </View>
        </View>

        {/* 手柄配对。角钮页，容器同亮度调节。 */}
        <View class={isCardVisible(slot('pairing')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('pairing'))} class="absolute left-0 top-0 w-[256] h-[256]">
            <PairingPage
              active={() => isPageActive(slot('pairing'))}
              interactive={interactive(slot('pairing'))}
            />
          </View>
        </View>

        {/* 电源管理。角钮页，容器同亮度调节。 */}
        <View class={isCardVisible(slot('power')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('power'))} class="absolute left-0 top-0 w-[256] h-[256]">
            <PowerPage
              active={() => isPageActive(slot('power'))}
              interactive={interactive(slot('power'))}
              onAskReboot={() => { rebootAsk.value = true; }}
              onAskPowerOff={() => { powerOffAsk.value = true; }}
            />
          </View>
        </View>

        {/* USB 模式：串口 / 手柄两档，切到「手柄」先弹确认 */}
        <View class={isCardVisible(slot('usbMode')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('usbMode'))} class={CENTER_BOX}>
            <ModePage
              active={() => isPageActive(slot('usbMode'))}
              interactive={interactive(slot('usbMode'))}
              onAskHost={() => { usbHostAsk.value = true; }}
            />
          </View>
        </View>

        {/* DS4、DS5 设置 */}
        <View class={isCardVisible(slot('dsSettings')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('dsSettings'))} class={CENTER_BOX}>
            <DsSettingsPage
              active={() => isPageActive(slot('dsSettings'))}
              interactive={interactive(slot('dsSettings'))}
            />
          </View>
        </View>

        {/* 系统信息 */}
        <View class={isCardVisible(slot('systemInfo')) ? CARD_BOX : 'hidden'}>
          <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
          <View nodeRef={setContentRef(slot('systemInfo'))} class={CENTER_BOX}>
            <SystemInfoPage
              active={() => isPageActive(slot('systemInfo'))}
              interactive={interactive(slot('systemInfo'))}
            />
          </View>
        </View>

        {/* 调试指令（仅开发构建） */}
        {IS_DEV ? (
          <View class={isCardVisible(slot('debug')) ? CARD_BOX : 'hidden'}>
            <Image src="main.svg" class="absolute left-0 top-0 w-[256] h-[256]" />
            <View nodeRef={setContentRef(slot('debug'))} class={CENTER_BOX}>
              <DebugPage
                active={() => isPageActive(slot('debug'))}
                interactive={interactive(slot('debug'))}
              />
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

      {/* 切到「手柄」确认弹窗：切过去之后 PC 上的串口消失，只能从本屏幕切回 */}
      {usbHostAsk.value ? (
        <ConfirmDialog
          title="切换到手柄模式"
          lines={['PC 的串口会消失', '只能在本屏幕切回或重启设备']}
          confirmLabel="切换"
          tone="primary"
          onCancel={() => { usbHostAsk.value = false; }}
          onConfirm={confirmUsbHost}
        />
      ) : null}

      {/* 切回串口后的重启询问：复位是 COM 口一定回来的那条路 */}
      {hw.usbRebootAsk ? (
        <ConfirmDialog
          title="已切回串口"
          lines={['是否立即重启设备？', '重启后 PC 的串口一定回来']}
          confirmLabel="立即重启"
          onCancel={() => { hw.usbRebootAsk = false; }}
          onConfirm={() => {
            hw.usbRebootAsk = false;
            rebootDevice();
          }}
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

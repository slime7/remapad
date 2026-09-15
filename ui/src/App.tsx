/**
 * Remapad 屏幕应用壳：顶部状态栏（半透明覆盖层）+ 功能页 + 悬浮底部菜单。
 * 功能页状态来自产品控制面（bridge），见 hooks/useHardware.ts。
 *
 * 七个页面在首次渲染里一次挂完：建树是同步阻塞的，分帧补挂会让首帧之后仍有
 * 数秒的建树期，期间每帧被阻塞、切页与滚动都在等建树。首屏因此推迟到全部页面
 * 就绪之后，这段等待由固件侧启动画面覆盖（见 docs/adr/0016）。切页只翻转
 * hidden（display:none），建好的页面不再重建；页面根节点自己负责 hidden 切换。
 *
 * 手柄操控模式（hooks/usePadControl.ts）下，页面与底栏的可聚焦性跟着「自己是
 * 当前页、且没有弹窗盖住」走：框架的焦点遍历清单因此只含画面上的控件，隐藏页
 * 与弹窗背后的按钮都不会被圆圈键按到。
 * 两棵子树还要各交一个节点引用给这个钩子：上下键绑在页面容器上、左右键绑在
 * 底栏上，方向键从此各走各的一摊（见 ADR 0028）。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';
import { AppStatusBar } from './components/AppStatusBar';
import { AppNavBar, type TabKey } from './components/AppNavBar';
import { ConfirmDialog } from './components/ConfirmDialog';
import { PadControlHint } from './components/PadControlHint';
import { useHardware, hw, powerOffDevice, rebootDevice } from './hooks/useHardware';
import { usePadControl } from './hooks/usePadControl';
import { ref } from 'vue';
import { HomePage } from './pages/HomePage';
import { SettingsPage } from './pages/SettingsPage';
import { ControllerSettingsPage } from './pages/ControllerSettingsPage';
import { PairingPage } from './pages/PairingPage';
import { ModePage } from './pages/ModePage';
import { SystemPage } from './pages/SystemPage';
import { DebugPage } from './pages/DebugPage';
import { CHARSET_ANCHOR, COLOR, STYLE } from './theme';

// 构建期字符集锚点：保持导入即可，让动态数字/符号字形进入字体图集。
void CHARSET_ANCHOR;

export default function App() {
  useHardware();
  const tab = ref<TabKey>('home');
  const rebootAsk = ref(false);
  const powerOffAsk = ref(false);

  const dialogOpen = () => rebootAsk.value || powerOffAsk.value;
  /* 页面与底栏的「可交互」：自己是当前页（底栏恒为真）、且没有弹窗盖住。
   * 弹窗打开时页面仍然可见，但焦点该留在弹窗里，所以一并关掉。 */
  const interactive = (key: TabKey) => () => tab.value === key && !dialogOpen();

  /* 手柄操控的两个轴各绑一棵子树：页面容器归上下键、底栏归左右键。节点在挂载
   * 时才到位，这里先留变量，钩子按帧取（见 usePadControl）。 */
  let pageRoot: NodeMirror | null = null;
  let navRoot: NodeMirror | null = null;
  const padPageRef = (node: NodeMirror | null) => {
    pageRoot = node;
  };
  const padNavRef = (node: NodeMirror | null) => {
    navRoot = node;
  };

  /* 手柄操控的焦点窗口与「非操控状态不留焦点环」也由这个钩子承担。 */
  usePadControl({
    firmwareMode: () => hw.padUiMode,
    pageRoot: () => pageRoot,
    navRoot: () => navRoot,
  });

  /** 切页：所有页面已挂载，只翻转 hidden。 */
  const goToTab = (next: TabKey) => {
    tab.value = next;
  };

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
      <AppStatusBar />
      <View nodeRef={padPageRef} class="w-full h-full overflow-hidden">
        {/* 每个页面自己带 hidden 切换，省掉一层纯容器节点（每个节点约 50 ms）。 */}
        <HomePage
          active={() => tab.value === 'home'}
          interactive={interactive('home')}
          onGo={goToTab}
        />
        <SettingsPage
          active={() => tab.value === 'settings'}
          interactive={interactive('settings')}
          onGo={goToTab}
        />
        <ControllerSettingsPage
          active={() => tab.value === 'controller'}
          interactive={interactive('controller')}
        />
        <PairingPage
          active={() => tab.value === 'pairing'}
          interactive={interactive('pairing')}
        />
        <ModePage
          active={() => tab.value === 'mode'}
          interactive={interactive('mode')}
        />
        <SystemPage
          active={() => tab.value === 'system'}
          interactive={interactive('system')}
          onAskReboot={() => (rebootAsk.value = true)}
          onAskPowerOff={() => (powerOffAsk.value = true)}
        />
        <DebugPage active={() => tab.value === 'debug'} interactive={interactive('debug')} />
      </View>
      <AppNavBar
        tab={tab.value}
        onChange={goToTab}
        enabled={() => !dialogOpen()}
        rootRef={padNavRef}
      />
      <PadControlHint active={() => hw.padUiMode} />

      {rebootAsk.value ? (
        <ConfirmDialog
          title="重启设备？"
          lines={['重启后回到 COM 设备模式，', '用于烧录与串口日志。']}
          confirmLabel="重启"
          onCancel={() => (rebootAsk.value = false)}
          onConfirm={confirmReboot}
        />
      ) : null}

      {powerOffAsk.value ? (
        <ConfirmDialog
          title="关机？"
          lines={['按 PWR 键可重新开机；', 'USB 供电下不会断电。']}
          confirmLabel="关机"
          onCancel={() => (powerOffAsk.value = false)}
          onConfirm={confirmPowerOff}
        />
      ) : null}

      {hw.rebooting || hw.poweringOff ? (
        <View class={STYLE.busyOverlay}>
          <Text class="text-sm" style={{ textColor: COLOR.onSurface }}>
            {hw.rebooting ? '重启中' : '关机中'}
          </Text>
        </View>
      ) : null}
    </View>
  );
}

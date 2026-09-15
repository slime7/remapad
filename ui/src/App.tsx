/**
 * Remapad 屏幕应用壳：顶部状态栏（半透明覆盖层）+ 功能页 + 悬浮底部菜单。
 * 功能页状态来自产品控制面（bridge），见 hooks/useHardware.ts。
 *
 * 七个页面在首次渲染里一次挂完：建树是同步阻塞的，分帧补挂会让首帧之后仍有
 * 数秒的建树期，期间每帧被阻塞、切页与滚动都在等建树。首屏因此推迟到全部页面
 * 就绪之后，这段等待由固件侧启动画面覆盖（见 docs/adr/0016）。切页只翻转
 * hidden（display:none），建好的页面不再重建；页面根节点自己负责 hidden 切换。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { AppStatusBar } from './components/AppStatusBar';
import { AppNavBar, type TabKey } from './components/AppNavBar';
import { ConfirmDialog } from './components/ConfirmDialog';
import { useHardware, hw, powerOffDevice, rebootDevice } from './hooks/useHardware';
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
      <View class="w-full h-full overflow-hidden">
        {/* 每个页面自己带 hidden 切换，省掉一层纯容器节点（每个节点约 50 ms）。 */}
        <HomePage active={() => tab.value === 'home'} onGo={goToTab} />
        <SettingsPage active={() => tab.value === 'settings'} onGo={goToTab} />
        <ControllerSettingsPage active={() => tab.value === 'controller'} />
        <PairingPage active={() => tab.value === 'pairing'} />
        <ModePage active={() => tab.value === 'mode'} />
        <SystemPage
          active={() => tab.value === 'system'}
          onAskReboot={() => (rebootAsk.value = true)}
          onAskPowerOff={() => (powerOffAsk.value = true)}
        />
        <DebugPage active={() => tab.value === 'debug'} />
      </View>
      <AppNavBar tab={tab.value} onChange={goToTab} />

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

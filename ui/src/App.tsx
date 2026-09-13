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
import { useHardware, hw, rebootDevice } from './hooks/useHardware';
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
  /** 配对进行中锁定底部导航，保证流程在配对页内完成。 */
  const pairingBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';

  /** 切页：所有页面已挂载，只翻转 hidden。 */
  const goToTab = (next: TabKey) => {
    tab.value = next;
  };

  const confirmReboot = () => {
    rebootAsk.value = false;
    rebootDevice();
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
        <SystemPage active={() => tab.value === 'system'} onAskReboot={() => (rebootAsk.value = true)} />
        <DebugPage active={() => tab.value === 'debug'} />
      </View>
      <AppNavBar tab={tab.value} disabled={pairingBusy} onChange={goToTab} />

      {/* 重启确认：官方 Modal 的 portal 层按 480x272 fallback 视口定位，
          在 240x280 上会错位，这里用本应用的绝对定位遮罩实现。 */}
      {rebootAsk.value ? (
        <View class={STYLE.scrim}>
          <View class={STYLE.modalBox}>
            <Text class="text-base font-bold" style={{ textColor: COLOR.onSurface }}>
              重启设备？
            </Text>
            <Text class="text-xs text-center mt-1" style={{ textColor: COLOR.onSurfaceVariant }}>
              重启后回到 COM 设备模式，
            </Text>
            <Text class="text-xs text-center" style={{ textColor: COLOR.onSurfaceVariant }}>
              用于烧录与串口日志。
            </Text>
            <View class="flex-row gap-2 mt-3">
              <View
                focusable
                onPress={() => (rebootAsk.value = false)}
                class={STYLE.modalCancelBtn}
              >
                <Text class="text-sm" style={{ textColor: COLOR.onSurface }}>
                  取消
                </Text>
              </View>
              <View focusable onPress={confirmReboot} class={STYLE.modalDangerBtn}>
                <Text class="text-sm font-bold" style={{ textColor: COLOR.onErrorContainer }}>
                  重启
                </Text>
              </View>
            </View>
          </View>
        </View>
      ) : null}

      {hw.rebooting ? (
        <View class={STYLE.busyOverlay}>
          <Text class="text-sm" style={{ textColor: COLOR.onSurface }}>
            重启中
          </Text>
        </View>
      ) : null}
    </View>
  );
}

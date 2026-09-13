/**
 * Remapad 屏幕应用壳：顶部状态栏（半透明覆盖层）+ 功能页 + 悬浮底部菜单。
 * 功能页状态来自产品控制面（bridge），见 hooks/useHardware.ts。
 *
 * 首帧只挂壳、状态栏、底栏与首页；其余页面按需挂载——首次进入时才建树，
 * 并交给页面内部的 useMountCursor 逐帧自上而下填充（每节点约 50 ms，见
 * docs/adr/0014-page-mount-on-demand-progressive-fill.md）。填充期间首页留白
 * 处显示加载提示，切页只翻转 hidden（display:none），建好的页面不再重建。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { AppStatusBar } from './components/AppStatusBar';
import { AppNavBar, type TabKey } from './components/AppNavBar';
import { useHardware, hw, rebootDevice } from './hooks/useHardware';
import { pagesFilling } from './hooks/useProgressiveMount';
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
  const mountedTabs = ref<readonly TabKey[]>(['home']);
  /** 配对进行中锁定底部导航，保证流程在配对页内完成。 */
  const pairingBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';
  /** 首页加载提示：有页面正在分帧填充时显示（按需挂载后通常出现在刚进页面时）。 */
  const pagesLoading = () => pagesFilling();

  /** 挂载一页（首次进入时建外层容器，内部由页面游标逐帧填充）；已挂载时为空操作。 */
  const mountTab = (next: TabKey) => {
    if (mountedTabs.value.includes(next)) return;
    mountedTabs.value = [...mountedTabs.value, next];
  };

  /** 切页：目标页还没挂过就先建外层容器，页面内部再逐帧填充。 */
  const goToTab = (next: TabKey) => {
    mountTab(next);
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
        <HomePage active={() => tab.value === 'home'} loading={pagesLoading} onGo={goToTab} />
        {mountedTabs.value.includes('settings') ? (
          <SettingsPage active={() => tab.value === 'settings'} onGo={goToTab} />
        ) : null}
        {mountedTabs.value.includes('controller') ? (
          <ControllerSettingsPage active={() => tab.value === 'controller'} />
        ) : null}
        {mountedTabs.value.includes('pairing') ? (
          <PairingPage active={() => tab.value === 'pairing'} />
        ) : null}
        {mountedTabs.value.includes('mode') ? (
          <ModePage active={() => tab.value === 'mode'} />
        ) : null}
        {mountedTabs.value.includes('system') ? (
          <SystemPage active={() => tab.value === 'system'} onAskReboot={() => (rebootAsk.value = true)} />
        ) : null}
        {mountedTabs.value.includes('debug') ? (
          <DebugPage active={() => tab.value === 'debug'} />
        ) : null}
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

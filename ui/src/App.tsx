/**
 * Remapad 屏幕应用壳：顶部状态栏（半透明覆盖层）+ 功能页 + 悬浮底部菜单。
 * 功能页状态来自产品控制面（bridge），见 hooks/useHardware.ts。
 *
 * 首帧只挂壳、状态栏、底栏与首页，其余页面在首帧之后每帧补挂一页：一次性
 * 挂载全部页面会把首帧推到 18 秒以上（每节点约 50 ms，见
 * docs/adr/0015-restore-deferred-page-mount-after-first-frame.md）。切页只翻转
 * hidden（display:none），建好的页面不再重建；页面根节点自己负责 hidden 切换。
 */
import { after } from '@pocketjs/framework/vue-vapor/clock';
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

/** 首页之外的页面：先补首页可直达的（两个圆钮 + 底栏设置键），其余按挂载耗时从短到长。 */
const DEFERRED_TABS: readonly TabKey[] = ['pairing', 'mode', 'settings', 'debug', 'controller', 'system'];
/** 首帧提交后再开始补挂，避免与首页首帧挤在同一帧（0.05 s ≈ 3 帧）。 */
const DEFER_START_SECONDS = 0.05;

export default function App() {
  useHardware();
  const tab = ref<TabKey>('home');
  const rebootAsk = ref(false);
  const mountedTabs = ref<readonly TabKey[]>(['home']);
  const pendingTabs = ref<readonly TabKey[]>(DEFERRED_TABS);
  /** 配对进行中锁定底部导航，保证流程在配对页内完成。 */
  const pairingBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';

  /** 挂载一页并移出待挂队列；已挂载时为空操作。 */
  const mountTab = (next: TabKey) => {
    if (mountedTabs.value.includes(next)) return;
    mountedTabs.value = [...mountedTabs.value, next];
    pendingTabs.value = pendingTabs.value.filter((candidate) => candidate !== next);
  };

  /** 每帧只补一页：单页建树本身就是秒级阻塞，合并批次只会让阻塞更长。 */
  const pumpDeferredMount = () => {
    const next = pendingTabs.value[0];
    if (next === undefined) return;
    mountTab(next);
    after(0, pumpDeferredMount);
  };
  after(DEFER_START_SECONDS, pumpDeferredMount);

  /** 切页：目标页还没补挂就立即挂上，不让用户停在空页上。 */
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
        <HomePage active={() => tab.value === 'home'} onGo={goToTab} />
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

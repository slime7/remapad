/**
 * Remapad 屏幕应用壳：顶部状态栏（半透明覆盖层）+ 功能页 + 悬浮底部菜单。
 * 功能页状态来自产品控制面（bridge），见 hooks/useHardware.ts。
 *
 * 所有页面常驻挂载，切换只翻转 hidden（display:none）——比条件挂载快，
 * 不用每次重建节点树和重新上传图片纹理。
 */
import { ref } from 'vue';
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { AppStatusBar } from './components/AppStatusBar';
import { AppNavBar, type TabKey } from './components/AppNavBar';
import { useHardware, hw, rebootDevice } from './hooks/useHardware';
import { HomePage } from './pages/HomePage';
import { SettingsPage } from './pages/SettingsPage';
import { PairingPage } from './pages/PairingPage';
import { ModePage } from './pages/ModePage';
import { SystemPage } from './pages/SystemPage';
import { DebugPage } from './pages/DebugPage';
import { CHARSET_ANCHOR } from './theme';

// 构建期字符集锚点：保持导入即可，让动态数字/符号字形进入字体图集。
void CHARSET_ANCHOR;

export default function App() {
  useHardware();
  const tab = ref<TabKey>('home');
  const rebootAsk = ref(false);
  /** 配对进行中锁定底部导航，保证流程在配对页内完成。 */
  const pairingBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';

  const confirmReboot = () => {
    rebootAsk.value = false;
    rebootDevice();
  };

  return (
    <View class="w-full h-full relative bg-[#060f1b] overflow-hidden">
      <AppStatusBar />
      <View class="w-full h-full overflow-hidden">
        <View class={tab.value === 'home' ? 'w-full h-full' : 'hidden'}>
          <HomePage active={() => tab.value === 'home'} onGo={(next) => (tab.value = next)} />
        </View>
        <View class={tab.value === 'settings' ? 'w-full h-full' : 'hidden'}>
          <SettingsPage active={() => tab.value === 'settings'} onGo={(next) => (tab.value = next)} />
        </View>
        <View class={tab.value === 'pairing' ? 'w-full h-full' : 'hidden'}>
          <PairingPage />
        </View>
        <View class={tab.value === 'mode' ? 'w-full h-full' : 'hidden'}>
          <ModePage active={() => tab.value === 'mode'} />
        </View>
        <View class={tab.value === 'system' ? 'w-full h-full' : 'hidden'}>
          <SystemPage active={() => tab.value === 'system'} onAskReboot={() => (rebootAsk.value = true)} />
        </View>
        <View class={tab.value === 'debug' ? 'w-full h-full' : 'hidden'}>
          <DebugPage active={() => tab.value === 'debug'} />
        </View>
      </View>
      <AppNavBar tab={tab.value} disabled={pairingBusy} onChange={(next) => (tab.value = next)} />

      {/* 重启确认：官方 Modal 的 portal 层按 480x272 fallback 视口定位，
          在 240x280 上会错位，这里用本应用的绝对定位遮罩实现。 */}
      {rebootAsk.value ? (
        <View class="absolute inset-0 z-50 flex-col items-center justify-center bg-[#000000b3]">
          <View class="w-[204] rounded-[16] bg-[#102035] p-3 flex-col items-center">
            <Text class="text-base font-bold text-[#d9e6ff]">重启设备？</Text>
            <Text class="text-xs text-[#9aacca] text-center mt-1">重启后回到 COM 设备模式，</Text>
            <Text class="text-xs text-[#9aacca] text-center">用于烧录与串口日志。</Text>
            <View class="flex-row gap-2 mt-3">
              <View
                focusable
                onPress={() => (rebootAsk.value = false)}
                class="w-[84] h-[40] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150"
              >
                <Text class="text-sm text-[#d9e6ff]">取消</Text>
              </View>
              <View
                focusable
                onPress={confirmReboot}
                class="w-[84] h-[40] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150"
              >
                <Text class="text-sm font-bold text-[#ff9993]">重启</Text>
              </View>
            </View>
          </View>
        </View>
      ) : null}

      {hw.rebooting ? (
        <View class="absolute inset-0 z-50 flex-row items-center justify-center bg-[#000000]">
          <Text class="text-sm text-[#d9e6ff]">重启中</Text>
        </View>
      ) : null}
    </View>
  );
}

/**
 * DS4、DS5 设置页（第 5 页，系统信息页之前）：
 * 位于四叶草中心区域 (130 × 130)。
 * 布局：标题 + 两行「选项名 + 开关」，开关见 components/Switch；屏幕上只放
 * 选项名，行为说明在外部文档里（触摸板加减：左半减号、右半加号；截图键：
 * 开着时触摸板按下发截图，关掉改发减号，见 docs/adr/0047-ds-behavior-settings.md）。
 * 两项设置经 bridge 落盘到固件 NVS，由数据面在触摸板按下时生效，
 * 见 firmware/main/pad/ds_behavior.h。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Switch } from '../components/Switch';
import { hw, setDsBehavior } from '../hooks/useHardware';
import { COLOR } from '../theme';

export function DsSettingsPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const toggle = (key: 'touchpadPlusMinus' | 'captureKey') => {
    setDsBehavior({ ...hw.dsBehavior, [key]: !hw.dsBehavior[key] });
  };

  return (
    <View class={props.active() ? 'w-full h-full flex-col items-center justify-center p-2 gap-[10]' : 'hidden'}>
      <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
        DS4、DS5 设置
      </Text>
      <View class="flex-row items-center justify-between w-full px-1 shrink-0">
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
          触摸板加减
        </Text>
        <Switch
          on={() => hw.dsBehavior.touchpadPlusMinus}
          onToggle={() => toggle('touchpadPlusMinus')}
          interactive={props.interactive}
        />
      </View>
      <View class="flex-row items-center justify-between w-full px-1 shrink-0">
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
          截图键
        </Text>
        <Switch
          on={() => hw.dsBehavior.captureKey}
          onToggle={() => toggle('captureKey')}
          interactive={props.interactive}
        />
      </View>
    </View>
  );
}

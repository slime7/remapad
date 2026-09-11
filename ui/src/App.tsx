import { ref, watchEffect, onMounted } from 'vue';
import {
  View,
  Text,
  Image,
  type NodeMirror,
} from '@pocketjs/framework/vue-vapor/components';
import { animate } from '@pocketjs/framework/animation';
import { TICKS_PER_SECOND } from '@pocketjs/framework/clock';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { frameworkName } from '@pocketjs/framework/vue-vapor';

const SPINNER_FRAME_STEP = 3;
const SPINNER_FRAMES = [
  'spinner-00.svg',
  'spinner-01.svg',
  'spinner-02.svg',
  'spinner-03.svg',
  'spinner-04.svg',
  'spinner-05.svg',
  'spinner-06.svg',
  'spinner-07.svg',
];

function Stat(props: { label: string; value: string; cls: string }) {
  return (
    <View class="flex-col items-end">
      <Text class={props.cls}>{props.value}</Text>
      <Text class="text-xs text-slate-500 tracking-wide">{props.label}</Text>
    </View>
  );
}

interface HeroProps {
  actionLabel?: string;
  deviceLabel?: string;
  headline?: string;
  onAction?: (count: number) => void;
  presentationHz?: number;
  runtimeLabel?: string;
  spinnerFrameStep?: number;
}

export default function Hero(props: HeroProps = {}) {
  const count = ref(0);
  let underline: NodeMirror | null = null;

  watchEffect(() => {
    const completedCount = count.value;
    if (completedCount > 0) props.onAction?.(completedCount);
  });

  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, {
    frameStep: props.spinnerFrameStep ?? SPINNER_FRAME_STEP,
  });

  onMounted(() => {
    if (underline) {
      animate(underline, 'width', 200, {
        dur: 700,
        easing: 'out',
        delay: 150,
      });
    }
  });

  return (
    <View class="w-full h-full flex-col items-stretch bg-[#060f1b]">
      <View class="status-bar">
        <View class="px-8 py-2 w-full flex">
          <Text class="text-[#d9e6ff]">汉字</Text>
          <View class="flex-1"></View>
          <Text class="text-[#d9e6ff]">68%</Text>
        </View>
      </View>

      <View class="flex-1"></View>

      <View class="px-4 pb-6 mt-4">
        <Text class="text-[#d9e6ff] text-xl">bottom</Text>
      </View>
    </View>
  );
}

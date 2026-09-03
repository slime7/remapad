import { ref, watchEffect, onMounted } from 'vue';
import { View, Text, Image } from '@pocketjs/framework/vue-vapor/components';
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

function Stat(props) {
  return (
    <View class="flex-col items-end">
      <Text class={props.cls}>{props.value}</Text>
      <Text class="text-xs text-slate-500 tracking-wide">{props.label}</Text>
    </View>
  );
}

export default function Hero(props = {}) {
  const count = ref(0);
  let underline = null;

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
    <View class="w-full h-full flex-col justify-between p-3 bg-gradient-to-b from-slate-50 to-slate-100">
      <View class="flex-col items-start gap-2">
        <View class="flex-row items-center gap-2">
          <Image class="w-8 h-8 rounded-lg shadow" src="logo.png" />
          <View class="flex-col">
            <Text class="text-sm text-slate-950 font-bold tracking-wide">
              PocketJS
            </Text>
            <Text class="text-xs text-slate-500 tracking-wide">
              {frameworkName()} + {props.runtimeLabel ?? 'RUST + SCEGU'}
            </Text>
          </View>
        </View>

        <View class="flex-row gap-2">
          <Stat
            label="FPS"
            value={String(props.presentationHz ?? TICKS_PER_SECOND)}
            cls="text-base text-emerald-600 font-bold"
          />
          <Stat
            label="NODES"
            value="42"
            cls="text-base text-blue-600 font-bold"
          />
          <Stat
            label="DRAWS"
            value="9"
            cls="text-base text-amber-600 font-bold"
          />
        </View>
      </View>

      <View class="flex-col gap-2">
        <Text class="text-xs text-blue-600 tracking-wide">
          ONE RUST CORE · ONE JSX APP
        </Text>
        <View class="flex-row items-center">
          <Text class="text-xl text-slate-950 font-bold">
            {props.headline ?? ('JSX at ' + TICKS_PER_SECOND + ' FPS.')}
          </Text>
          <Image class="w-8 h-8" src={spinnerSrc.value} />
        </View>
        <View
          nodeRef={(el) => { underline = el; }}
          class="h-1 w-0 rounded-full shadow bg-gradient-to-r from-blue-500 to-cyan-500"
          style={{ translateX: count.value * 2 }}
        />
        <View class="flex-col gap-0.5">
          <Text class="text-xs text-slate-600">
            Flexbox, springs and baked type —
          </Text>
          <Text class="text-xs text-slate-600">
            {props.deviceLabel ?? 'running on a 2005 handheld.'}
          </Text>
        </View>
      </View>

      <View class="flex-col items-start gap-2">
        <View
          class="px-4 py-2 rounded-xl shadow-md bg-blue-600 border-blue-500 active:bg-blue-700 transition-colors duration-150"
          focusable
          onPress={() => { count.value++; }}
        >
          <Text class="text-sm text-white font-bold">
            {props.actionLabel ?? 'Press Circle'}
          </Text>
        </View>

        <View class="flex-row items-center gap-3">
          <Text class="text-xs text-slate-600">
            Count: {count.value}
          </Text>
          <Text class="text-xs text-emerald-600 font-bold">
            {count.value > 3 ? 'Reactive on real hardware.' : ''}
          </Text>
        </View>
      </View>
    </View>
  );
}

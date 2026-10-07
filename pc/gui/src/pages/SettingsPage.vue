<!-- 设置页：设备屏幕上的可改项搬到 PC；控件值全部来自固件回读，设备是唯一事实源。 -->
<template>
  <div class="grid gap-3 content-start grid-cols-[repeat(auto-fit,minmax(min(100%,380px),1fr))] pb-3">
    <mat-card>
      <template #headline>
        屏幕与亮度
      </template>
      <mat-card-content class="space-y-2">
        <div class="flex flex-wrap items-center gap-2">
          <span class="shrink-0 text-mat-on-surface-variant">亮度</span>
          <mat-slider
            v-model="brightness"
            class="grow shrink basis-40 min-w-[140px]"
            :min="BRIGHTNESS_MIN"
            :max="BRIGHTNESS_MAX"
            aria-label="屏幕亮度"
            show-value-indicator
            @change="applyBrightness"
          />
          <span class="shrink-0 min-w-[2.5em] text-end tabular-nums">{{ brightness }}</span>
        </div>
        <mat-switch
          :model-value="state.settings.screenOn"
          :disabled="!state.connected"
          @update:model-value="setScreen"
        >
          息屏
        </mat-switch>
        <p class="text-mat-on-surface-variant text-xs">
          息屏只关背光；亮度改完即落盘，调亮度会顺带亮屏。
        </p>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        电源
      </template>
      <mat-card-content class="space-y-2">
        <div class="flex flex-wrap items-center gap-2">
          <mat-btn class="grow shrink basis-[120px]" variant="outlined" :disabled="!state.connected" @click="askReboot">
            重启设备
          </mat-btn>
          <mat-btn
            class="grow shrink basis-[120px]"
            variant="filled"
            color="error"
            :disabled="!state.connected"
            @click="askPoweroff"
          >
            设备关机
          </mat-btn>
        </div>
        <p class="text-mat-on-surface-variant text-xs">
          reboot 重启回 COM 模式；poweroff 释放电源锁存，USB 供电下关不掉。
        </p>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        设备信息
      </template>
      <mat-card-content class="space-y-2">
        <div class="flex flex-wrap items-center justify-end gap-2">
          <mat-btn variant="outlined" :disabled="!state.connected" @click="readSettings">
            读取当前设置
          </mat-btn>
        </div>
        <pre class="[font:inherit] whitespace-pre-wrap text-mat-on-surface-variant">{{ state.factsText }}</pre>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        手柄配色
      </template>
      <mat-card-content class="space-y-2">
        <div class="flex flex-wrap gap-2">
          <button
            v-for="way in COLORWAYS"
            :key="way[0]"
            type="button"
            class="grow shrink basis-[88px] h-[34px] px-2.5 text-[13px] rounded-mat-medium
                   cursor-pointer disabled:opacity-50 disabled:cursor-default"
            :style="{ background: cssColor(way[1]), color: readableOn(way[1]) }"
            :disabled="!state.connected"
            @click="applyColors([way[1], way[2], way[3], way[4]])"
          >
            {{ way[0] }}
          </button>
        </div>
        <div class="grid gap-2 grid-cols-[repeat(auto-fit,minmax(110px,1fr))]">
          <label
            v-for="(name, index) in COLOR_FIELDS"
            :key="name"
            class="flex flex-col gap-0.5 text-xs text-mat-on-surface-variant"
          >
            <span>{{ name }}</span>
            <mat-text-field v-model="colorTexts[index]" :disabled="!state.connected" />
          </label>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <mat-btn variant="filled-tonal" :disabled="!state.connected" @click="applyCustomColors">
            应用配色
          </mat-btn>
          <span class="text-mat-on-surface-variant text-xs">{{ state.settings.colorSummary }}</span>
        </div>
        <p class="text-mat-on-surface-variant text-xs">
          四段依次是机身、按键、高光、握把，写 0xRRGGBB。改完设备会断链并重开连接窗口，主机自己连回来。
        </p>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        DS4、DS5 设置
      </template>
      <mat-card-content class="space-y-2">
        <div class="flex flex-wrap items-center gap-x-6 gap-y-2">
          <mat-switch
            :model-value="state.settings.dsTouchpad"
            :disabled="!state.connected"
            @update:model-value="(on) => sendDs('touchpad', on)"
          >
            触摸板加减
          </mat-switch>
          <mat-switch
            :model-value="state.settings.dsCapture"
            :disabled="!state.connected"
            @update:model-value="(on) => sendDs('capture', on)"
          >
            截图键
          </mat-switch>
        </div>
        <p class="text-mat-on-surface-variant text-xs">
          触摸板加减：左半区按下发减号，右半区发加号；截图键：触摸板按下发截图。两项都落盘在设备上。
        </p>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        WiFi 调试
      </template>
      <mat-card-content class="space-y-2">
        <mat-text-field v-model="ssid" class="w-full" label="SSID" :disabled="!state.connected" />
        <mat-text-field v-model="password" class="w-full" label="密码" type="password" :disabled="!state.connected" />
        <div class="flex flex-wrap items-center gap-2">
          <mat-btn variant="filled-tonal" :disabled="!state.connected" @click="saveWifi">
            保存
          </mat-btn>
          <span class="text-mat-on-surface-variant text-xs">{{ state.settings.netSummary }}</span>
        </div>
        <p class="text-mat-on-surface-variant text-xs">
          连上设备会自动回读已存的 SSID 与密码；保存只写入设备，不自动连接。SSID 与密码暂不支持空格。
          连接后在工具栏「网络」填 设备IP:{{ NETLOG_PORT_DEFAULT }}，就能远程转发手柄与看日志，仅限 2.4G 网络。
        </p>
      </mat-card-content>
    </mat-card>
  </div>
</template>

<script setup>
import { computed, ref, watch } from 'vue';
import { confirm } from 'mde-vue';

import {
  COLOR_FIELDS,
  COLORWAYS,
  SETTINGS_READ_COMMANDS,
} from '@remapad/pc/gui-constants';
import {
  BRIGHTNESS_MAX,
  BRIGHTNESS_MIN,
  NETLOG_PORT_DEFAULT,
  cssColor,
  parseColor,
  readableOn,
} from '@remapad/pc/gui-shared';
import { api } from '../api.js';
import { logLocal, store } from '../store.js';

const state = computed(() => store.state ?? {
  connected: false,
  factsText: '连接设备后自动读一次。',
  settings: {
    brightness: 60, screenOn: true, colorSummary: '当前：未读取',
    dsTouchpad: false, dsCapture: true, wifiSsid: '', wifiPass: '', netSummary: '设备：未读取',
  },
});

// 亮度本地值拖动中不被轮询回写：服务端值变化时，距上次用户编辑超过 2 秒才跟。
const brightness = ref(60);
let brightnessEditedAt = 0;
watch(() => state.value.settings.brightness, (value) => {
  if (Date.now() - brightnessEditedAt > 2000) {
    brightness.value = value;
  }
}, { immediate: true });

function applyBrightness() {
  brightnessEditedAt = Date.now();
  api.command(`backlight ${brightness.value}`).catch(() => {});
}

function setScreen(on) {
  state.value.settings.screenOn = on;
  api.command(on ? 'screen on' : 'screen off').catch(() => {});
}

async function askReboot() {
  if (await confirm({
    title: '确认重启？',
    content: '重启会结束当前会话，设备回来后要重新连接。',
    confirmText: '重启',
    cancelText: '取消',
  })) {
    api.command('reboot').catch(() => {});
  }
}

async function askPoweroff() {
  if (await confirm({
    title: '确认关机？',
    content: '关机会断开链路；USB 供电下设备会重新上电。',
    confirmText: '关机',
    cancelText: '取消',
  })) {
    api.command('poweroff').catch(() => {});
  }
}

function readSettings() {
  for (const command of SETTINGS_READ_COMMANDS) {
    api.command(command).catch(() => {});
  }
}

function applyColors(colors) {
  api.command(`ctrl ${colors.map((value) => `0x${value.toString(16).padStart(6, '0')}`).join(' ')}`).catch(() => {});
}

// 自定义配色的输入框：本地编辑，回读到达时同步（用户正在改就先不覆盖）。
const DEFAULT_COLOR_TEXTS = ['0x232323', '0xa0a0a0', '0xe6e6e6', '0x323232'];
const colorTexts = ref(DEFAULT_COLOR_TEXTS.slice());
let colorsEditedAt = 0;
watch(() => state.value.settings.colors, (colors) => {
  if (colors && Date.now() - colorsEditedAt > 2000) {
    colorTexts.value = colors.map((value) => `0x${value.toString(16).padStart(6, '0')}`);
  }
}, { immediate: true });

function applyCustomColors() {
  colorsEditedAt = Date.now();
  const colors = [];
  for (let index = 0; index < COLOR_FIELDS.length; index += 1) {
    const value = parseColor(colorTexts.value[index]);
    if (value === null) {
      logLocal(`${COLOR_FIELDS[index]}配色要写 0xRRGGBB，现在是 ${colorTexts.value[index] || '空'}`);
      return;
    }
    colors.push(value);
  }
  applyColors(colors);
}

function sendDs(key, on) {
  api.command(`ds ${key} ${on ? 'on' : 'off'}`).catch(() => {});
}

const ssid = ref('');
const password = ref('');
let wifiEditedAt = 0;
watch(() => state.value.settings, (settings) => {
  if (Date.now() - wifiEditedAt > 2000) {
    ssid.value = settings.wifiSsid;
    password.value = settings.wifiPass;
  }
}, { immediate: true });

function saveWifi() {
  wifiEditedAt = Date.now();
  const name = ssid.value.trim();
  if (!name || !password.value) {
    logLocal('SSID 与密码都要填');
    return;
  }
  if (name.includes(' ') || password.value.includes(' ')) {
    logLocal('SSID 与密码暂不支持空格');
    return;
  }
  api.wifiSave(name, password.value).catch(() => {});
}
</script>

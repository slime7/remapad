<!-- 会话页：转发开关、输入手柄选择与链路快捷动作。 -->
<template>
  <div class="grid gap-3 content-start grid-cols-[repeat(auto-fit,minmax(min(100%,340px),1fr))] pb-3">
    <mat-card>
      <template #headline>
        转发
      </template>
      <mat-card-content class="space-y-2">
        <mat-switch
          :model-value="state.forward"
          :disabled="!state.connected || !state.hidReady"
          @update:model-value="setForward"
        >
          转发手柄到设备
        </mat-switch>
        <p class="text-mat-on-surface-variant text-xs">
          默认开启；关掉后手柄不再上行，命令与截图照常。
        </p>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        输入手柄
      </template>
      <mat-card-content class="flex flex-wrap items-center gap-2">
        <mat-select
          :model-value="padValue"
          class="grow shrink basis-[220px] min-w-[200px]"
          label="输入手柄"
          :items="padItems"
          :disabled="!state.hidReady"
          @update:model-value="selectPad"
        />
        <mat-btn variant="outlined" :disabled="!state.hidReady" @click="refreshPads">
          刷新
        </mat-btn>
        <mat-btn variant="outlined" :disabled="!state.hidReady" @click="logCandidates">
          列出候选接口
        </mat-btn>
        <p class="w-full text-mat-on-surface-variant text-xs">
          {{ state.hidReady ? '列表与命令行 --list 相同，自动接入选中的那只。' : (state.hidProblem || 'hidapi 不可用，手柄转发关闭；串口命令与截图照常') }}
        </p>
      </mat-card-content>
    </mat-card>

    <mat-card>
      <template #headline>
        链路动作
      </template>
      <mat-card-content class="space-y-2">
        <p class="text-mat-on-surface-variant">
          {{ counters }}
        </p>
        <div class="flex flex-wrap gap-2">
          <mat-btn
            v-for="action in QUICK_ACTIONS"
            :key="action[1]"
            variant="filled-tonal"
            :disabled="!state.connected"
            @click="run(action[1])"
          >
            {{ action[0] }}
          </mat-btn>
        </div>
        <p class="text-mat-on-surface-variant text-xs">
          连接、屏幕与状态回读没有按钮，去「命令」页输入命令回车发送。
        </p>
      </mat-card-content>
    </mat-card>
  </div>
</template>

<script setup>
import { computed } from 'vue';

import { QUICK_ACTIONS } from '@remapad/pc/gui-constants';
import { api } from '../api.js';
import { store } from '../store.js';

const state = computed(() => store.state ?? {
  forward: true, connected: false, hidReady: false, hidProblem: '',
  pads: [], padIndex: null, counters: { reports: 0, frames: 0, outputs: 0 },
});

const AUTO_PAD = -1;
const padItems = computed(() => [
  { title: '自动挑第一只', value: AUTO_PAD },
  ...state.value.pads.map((pad, index) => ({ title: pad.describe, value: index })),
]);
const padValue = computed(() => state.value.padIndex ?? AUTO_PAD);
const counters = computed(() => {
  const { reports, frames, outputs } = state.value.counters;
  return `转发 ${reports} ｜ 设备帧 ${frames} ｜ 写回 ${outputs}`;
});

function setForward(enabled) {
  api.forward(enabled).catch(() => {});
}

function selectPad(value) {
  api.padSelect(value === AUTO_PAD ? null : value).catch(() => {});
}

function refreshPads() {
  api.refreshPads().catch(() => {});
}

function logCandidates() {
  api.padCandidates().catch(() => {});
}

function run(command) {
  api.command(command).catch(() => {});
}
</script>

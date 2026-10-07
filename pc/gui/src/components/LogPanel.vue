<!-- 日志面板：WS 推来的会话记录着色显示，带时间戳/自动滚动开关与清空/导出。 -->
<template>
  <section class="flex flex-none flex-col gap-1.5 h-[280px] pt-3 border-t border-mat-outline-variant">
    <div class="flex flex-wrap flex-none items-center gap-2 min-h-11">
      <mat-switch v-model="stamped">
        时间戳
      </mat-switch>
      <mat-switch v-model="autoscroll">
        自动滚动
      </mat-switch>
      <mat-spacer />
      <mat-btn variant="outlined" @click="clearLog">
        清空
      </mat-btn>
      <mat-btn variant="outlined" @click="exportLog">
        导出
      </mat-btn>
      <mat-btn variant="outlined" @click="sendHelp">
        :help
      </mat-btn>
    </div>
    <mat-scroll-area
      ref="area"
      class="flex-1 min-h-0 rounded-mat-medium"
      color="surface-container"
      no-scroll-padding
      aria-label="会话日志"
    >
      <div
        class="h-full py-2 px-2.5 box-border font-[Consolas,'Microsoft_YaHei_UI',monospace]
               text-xs leading-[1.55] text-mat-on-surface whitespace-pre-wrap break-all"
        aria-live="polite"
      >
        <div v-for="entry in store.log" :key="entry.id" :class="KIND_CLASS[entry.kind]">
          {{ lineText(entry) }}
        </div>
      </div>
    </mat-scroll-area>
  </section>
</template>

<script setup>
import { nextTick, ref, watch } from 'vue';

import { api } from '../api.js';
import { store } from '../store.js';

// 错误与发送行着色；其余行用正文色。
const KIND_CLASS = {
  error: 'text-[#e06c75]',
  send: 'text-[#7aa2f7]',
};

const area = ref(null);
const stamped = ref(true);
const autoscroll = ref(true);

function lineText(entry) {
  if (!stamped.value) {
    return entry.text;
  }
  const pad = (value) => String(value).padStart(2, '0');
  const at = entry.at;
  return `[${pad(at.getHours())}:${pad(at.getMinutes())}:${pad(at.getSeconds())}] ${entry.text}`;
}

watch(() => store.log.length, async () => {
  if (!autoscroll.value) {
    return;
  }
  await nextTick();
  const scroller = area.value?.getScroller();
  if (scroller) {
    scroller.scrollTop = scroller.scrollHeight;
  }
});

function clearLog() {
  store.log.splice(0);
}

function exportLog() {
  const text = store.log.map((entry) => lineText(entry)).join('\n') + '\n';
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = `remapad-log-${new Date().toISOString().replace(/[:T]/g, '-').slice(0, 19)}.txt`;
  link.click();
  URL.revokeObjectURL(url);
}

function sendHelp() {
  api.command(':help').catch(() => {});
}
</script>

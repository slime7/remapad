<!-- 命令页：调试口。常用命令按分组列成按钮（点了只填进输入框，回车才发）。 -->
<template>
  <div class="flex flex-col gap-2.5 pb-3">
    <div class="flex flex-wrap items-center gap-2">
      <mat-text-field
        v-model="store.command"
        class="grow shrink basis-[320px] min-w-[260px]"
        label="固件 CLI 命令"
        placeholder="输入固件命令，或 : 开头的工具命令；:help 看清单"
        :disabled="!connected"
        @keydown.enter.prevent="submitCommand"
        @keydown.up.prevent="recallHistory(-1)"
        @keydown.down.prevent="recallHistory(1)"
      />
      <mat-btn :disabled="!connected" @click="submitCommand">
        发送
      </mat-btn>
    </div>
    <p class="m-0 text-mat-on-surface-variant text-xs">
      回车发送，↑ / ↓ 取历史。下面的按钮只把命令填进输入框，回车才发出。
      回复与固件日志都落在底部日志区，切页不影响；设备命令全表用 help 命令查看。
    </p>

    <section v-for="[title, commands] in COMMAND_GROUPS" :key="title">
      <h3 class="mt-1 mb-1.5 text-[13px] font-semibold">
        {{ title }}
      </h3>
      <div class="flex flex-wrap gap-2">
        <mat-btn
          v-for="command in commands"
          :key="command"
          variant="outlined"
          size="small"
          @click="fill(command)"
        >
          {{ command }}
        </mat-btn>
      </div>
    </section>
  </div>
</template>

<script setup>
import { computed } from 'vue';

import { COMMAND_GROUPS } from '@remapad/pc/gui-constants';
import { recallHistory, store, submitCommand } from '../store.js';

const connected = computed(() => store.state?.connected ?? false);

function fill(command) {
  store.command = command;
}
</script>

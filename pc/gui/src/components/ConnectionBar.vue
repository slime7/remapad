<!-- 连接条：串口选择/连接与网络连接两条路，各管各的断开；同一时间只允许一个会话。 -->
<template>
  <div class="flex flex-none flex-wrap items-center gap-x-6 gap-y-2 py-2 border-b border-mat-outline-variant">
    <div class="flex grow flex-wrap items-center gap-2 min-w-0">
      <span class="shrink-0 text-mat-on-surface-variant">串口</span>
      <mat-select
        v-model="port"
        class="grow shrink basis-[150px] min-w-[130px] max-w-60"
        label="串口"
        placeholder="插上设备后点刷新"
        :items="portItems"
        :disabled="busy"
        aria-label="串口"
      />
      <mat-btn
        class="shrink-0"
        :variant="serialActive ? 'outlined' : 'filled'"
        :color="serialActive ? 'error' : undefined"
        :disabled="serialDisabled"
        :loading="busy"
        @click="toggleSerial"
      >
        {{ serialActive ? '断开' : '串口连接' }}
      </mat-btn>
      <mat-btn class="shrink-0" variant="outlined" :disabled="busy" @click="refreshPorts">
        刷新
      </mat-btn>
    </div>
    <div class="flex grow flex-wrap items-center gap-2 min-w-0">
      <span class="shrink-0 text-mat-on-surface-variant">网络</span>
      <mat-text-field
        v-model="netTarget"
        class="grow shrink basis-[180px] min-w-[170px] max-w-[260px]"
        label="设备IP:端口"
        :placeholder="`默认 ${NETLOG_PORT_DEFAULT}`"
        :disabled="busy"
      />
      <mat-btn
        class="shrink-0"
        :variant="netActive ? 'outlined' : 'filled'"
        :color="netActive ? 'error' : undefined"
        :disabled="netDisabled"
        :loading="busy"
        @click="toggleNet"
      >
        {{ netActive ? '断开' : '网络连接' }}
      </mat-btn>
    </div>
  </div>
</template>

<script setup>
import { computed, ref, watch } from 'vue';

import { NETLOG_PORT_DEFAULT } from '@remapad/pc/gui-shared';
import { api } from '../api.js';
import { store } from '../store.js';

const port = ref('');
const netTarget = ref('');
const busy = computed(() => store.state?.sessionState === 'connecting');
const connected = computed(() => store.state?.connected ?? false);
const netSession = computed(() => store.state?.netSession ?? false);

const serialActive = computed(() => connected.value && !netSession.value);
const netActive = computed(() => connected.value && netSession.value);
const serialDisabled = computed(() => busy.value || netActive.value);
const netDisabled = computed(() => busy.value || serialActive.value);

const portItems = computed(() => {
  const list = new Set(store.state?.ports ?? []);
  if (port.value) {
    list.add(port.value);
  }
  return [...list].map((name) => ({ title: name, value: name }));
});

// 端口选择落点由后端 portSelection 决定，这里只跟着快照走；用户手选后立刻回写后端没有
// 单独的 API——连接时带上所选值即可，刷新由后端守住用户的选择。
watch(() => store.state?.port, (value) => {
  if (value && !serialActive.value && !busy.value) {
    port.value = value;
  }
}, { immediate: true });

async function toggleSerial() {
  if (serialActive.value) {
    await api.disconnect();
    return;
  }
  await api.connectSerial(port.value.trim());
}

async function toggleNet() {
  if (netActive.value) {
    await api.disconnect();
    return;
  }
  await api.connectNet(netTarget.value.trim());
}

async function refreshPorts() {
  await api.refreshPorts();
}
</script>

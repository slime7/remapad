<!-- 升级页：本地校验镜像后推送固件；进度与结果由后端事件驱动快照给出。 -->
<template>
  <div class="flex flex-col gap-3 max-w-[760px] pb-3">
    <mat-text-field
      v-model="imagePath"
      class="w-full"
      label="镜像路径"
      placeholder="remapad_firmware.bin 的完整路径"
    />
    <div class="flex flex-wrap items-center gap-3">
      <mat-btn variant="outlined" @click="validate">
        校验镜像
      </mat-btn>
      <mat-btn variant="filled" :disabled="!connected" @click="start">
        开始升级
      </mat-btn>
      <mat-switch v-model="waitBack">
        升级完成后等设备回来并重新连接
      </mat-switch>
    </div>

    <mat-progress
      shape="wavy"
      wave-motion
      :value="state.ota.percent"
      aria-label="升级进度"
    />
    <p class="m-0 text-mat-on-surface-variant">
      {{ state.ota.label }}
    </p>

    <p class="m-0 text-mat-on-surface-variant text-xs">
      升级写入非运行分区，校验通过后设备自动重启，首次启动处于待验证状态。
      重启会结束当前会话，链路断开是正常现象；勾选上面的开关会自动等设备回到串口。
      串口与网络会话都能升级；WiFi 走 UDP，靠窗口重发兜丢包，比串口慢，重启后 netlog
      会话要重新打开，截图仍只走串口。升级期间不要关闭本工具的服务进程。
    </p>
  </div>
</template>

<script setup>
import { computed, ref, watch } from 'vue';

import { api } from '../api.js';
import { store } from '../store.js';

const state = computed(() => store.state ?? { connected: false, ota: { percent: 0, label: '未开始' }, defaultImage: '' });
const connected = computed(() => state.value.connected);

const imagePath = ref('');
watch(() => state.value.defaultImage, (value) => {
  // 只在还没填过路径时落默认值：用户手改后不被轮询覆盖。
  if (value && !imagePath.value) {
    imagePath.value = value;
  }
}, { immediate: true });

const waitBack = ref(true);

async function validate() {
  await api.otaValidate(imagePath.value.trim()).catch(() => {});
}

async function start() {
  await api.otaStart(imagePath.value.trim(), waitBack.value).catch(() => {});
}
</script>

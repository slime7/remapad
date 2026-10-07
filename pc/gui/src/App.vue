<!-- 连接控制台骨架：app-bar 登记顶边，正文是「连接条 + 页签 + 页面 + 日志」的纵向 flex。 -->
<template>
  <mat-app-root scrollable class="bg-mat-surface">
    <mat-app-bar app variant="small" content="headline">
      Remapad 连接控制台
      <template #trailing>
        <mat-btn
          :icon="themeIcon"
          :label="`配色模式：${themeLabel}`"
          variant="standard"
          size="small"
          @click="cycleTheme"
        />
        <span class="shrink-0 font-semibold">{{ stateStyle.label }}</span>
        <span class="shrink-0 ml-2 text-mat-on-surface-variant">{{ padSummary }}</span>
      </template>
    </mat-app-bar>

    <main class="flex flex-1 flex-col gap-2 w-full max-w-[1280px] mx-auto min-h-0 px-3 pb-2">
      <ConnectionBar />

      <nav class="flex shrink-0">
        <mat-btn-group
          variant="connected"
          selection="single"
          :selected="store.page"
          aria-label="页面切换"
          @select="store.page = $event.nextSelected"
        >
          <mat-btn value="session">
            会话
          </mat-btn>
          <mat-btn value="settings">
            设置
          </mat-btn>
          <mat-btn value="command">
            命令
          </mat-btn>
          <mat-btn value="upgrade">
            升级
          </mat-btn>
        </mat-btn-group>
      </nav>

      <section class="flex flex-1 flex-col min-h-0 overflow-y-auto">
        <SessionPage v-show="store.page === 'session'" />
        <SettingsPage v-show="store.page === 'settings'" />
        <CommandPage v-show="store.page === 'command'" />
        <UpgradePage v-show="store.page === 'upgrade'" />
      </section>

      <LogPanel />
    </main>
  </mat-app-root>
</template>

<script setup>
import { computed, onMounted } from 'vue';
import { useMatTheme } from 'mde-vue';

import { STATE_STYLE } from '@remapad/pc/gui-constants';
import ConnectionBar from './components/ConnectionBar.vue';
import LogPanel from './components/LogPanel.vue';
import CommandPage from './pages/CommandPage.vue';
import SessionPage from './pages/SessionPage.vue';
import SettingsPage from './pages/SettingsPage.vue';
import UpgradePage from './pages/UpgradePage.vue';
import { startBackend, store } from './store.js';

onMounted(startBackend);

const stateStyle = computed(() => STATE_STYLE[store.state?.sessionState ?? 'disconnected']);
const padSummary = computed(() => store.state?.padSummary ?? '手柄：未接入');

// 配色模式三态循环（跟随系统 → 浅色 → 深色），手选结果落 localStorage。
const theme = useMatTheme();
const THEME_CYCLE = [
  { mode: 'system', icon: 'brightness_auto', label: '跟随系统' },
  { mode: 'light', icon: 'light_mode', label: '浅色' },
  { mode: 'dark', icon: 'dark_mode', label: '深色' },
];
const themeIndex = computed(() => Math.max(0, THEME_CYCLE.findIndex((item) => item.mode === theme.mode.value)));
const themeIcon = computed(() => THEME_CYCLE[themeIndex.value].icon);
const themeLabel = computed(() => THEME_CYCLE[themeIndex.value].label);

function cycleTheme() {
  const next = THEME_CYCLE[(themeIndex.value + 1) % THEME_CYCLE.length];
  theme.setMode(next.mode);
  localStorage.setItem('remapad-theme', next.mode);
}
</script>

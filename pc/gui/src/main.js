// 入口：装 mde-vue 插件（主题 + 全局组件注册），挂到 #app。
import { createApp } from 'vue';
import { createMatUi } from 'mde-vue';
import './styles/app.css';
import App from './App.vue';

// 主题模式持久化由应用自管（mde-vue 不写 localStorage）：记住上次手选的浅色/深色。
const THEME_MODES = new Set(['light', 'dark', 'system']);
const themeMode = THEME_MODES.has(localStorage.getItem('remapad-theme') ?? '')
  ? localStorage.getItem('remapad-theme')
  : 'system';

createApp(App)
  .use(createMatUi({
    theme: {
      mode: themeMode,
      seedColor: '#20a6fc',
    },
  }))
  .mount('#app');

import '@pocketjs/framework/prelude';
import { mount } from '@pocketjs/framework/vue-vapor';
import Hero from './App.jsx';

mount(() => <Hero />, {
  pak: typeof globalThis !== 'undefined' ? globalThis.__pak : undefined,
});

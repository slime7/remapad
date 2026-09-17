import js from '@eslint/js';
import globals from 'globals';
import stylistic from '@stylistic/eslint-plugin';

export default [
  // ==================== 全局忽略 ====================
  {
    ignores: [
      'node_modules/**/*',
      'dist/**/*',
      'coverage/**/*',
      '**/*.d.ts',
      '**/*.bin',
      '**/*.pocket',
      'scripts/internal/**/*',
    ],
  },

  js.configs.recommended,

  // ==================== 插件 & 通用规则 ====================
  {
    files: ['**/*.{js,mjs,jsx}'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.node,
        ...globals.browser,
      },
      parserOptions: {
        ecmaFeatures: {
          jsx: true,
        },
      },
    },
    plugins: {
      '@stylistic': stylistic,
    },
    rules: {
      // ------- 代码风格 -------
      '@stylistic/semi': ['error', 'always'],
      '@stylistic/semi-spacing': ['error', { before: false, after: true }],
      '@stylistic/quotes': ['error', 'single', { avoidEscape: true }],
      '@stylistic/indent': ['error', 2, { SwitchCase: 0 }],
      '@stylistic/comma-dangle': ['error', {
        arrays: 'always-multiline',
        objects: 'always-multiline',
        imports: 'always-multiline',
        exports: 'always-multiline',
        functions: 'always-multiline',
      }],
      '@stylistic/object-curly-spacing': ['error', 'always'],
      '@stylistic/quote-props': ['error', 'as-needed', {
        keywords: false,
        unnecessary: true,
        numbers: false,
      }],

      // ------- 空格与空白 -------
      '@stylistic/keyword-spacing': ['error', {
        before: true,
        after: true,
        overrides: {
          return: { after: true },
          throw: { after: true },
          case: { after: true },
        },
      }],
      '@stylistic/space-before-function-paren': ['error', {
        anonymous: 'always',
        named: 'never',
        asyncArrow: 'always',
      }],
      '@stylistic/space-unary-ops': ['error', { words: true, nonwords: false }],
      '@stylistic/space-in-parens': ['error', 'never'],
      '@stylistic/no-multi-spaces': ['error', { ignoreEOLComments: false }],
      '@stylistic/no-trailing-spaces': 'error',
      '@stylistic/eol-last': ['error', 'always'],
      '@stylistic/no-multiple-empty-lines': ['error', { max: 1, maxEOF: 0, maxBOF: 0 }],

      // ------- 变量与语法 -------
      'no-unused-vars': ['error', { caughtErrors: 'none' }],
      'no-param-reassign': ['error', { props: true }],

      // ------- 关闭的规则 -------
      '@stylistic/max-len': 'off',
      'max-len': 'off',
      'no-unsafe-optional-chaining': 'off',
    },
  },
];

// PC 侧 JS 项目的 eslint 规则：沿用 ui/eslint.config.mjs 的历史规则（PocketJS 时代），
// 缩进与 .editorconfig 一致取 2；Vue SFC 走 eslint-plugin-vue 强推荐集。
import js from '@eslint/js';
import globals from 'globals';
import stylistic from '@stylistic/eslint-plugin';
import vue from 'eslint-plugin-vue';

export default [
    {
        ignores: ['**/node_modules/**', '**/dist/**', 'pc/shots/**'],
    },

    js.configs.recommended,
    ...vue.configs['flat/strongly-recommended'],

    {
        files: ['**/*.{js,mjs,vue}'],
        languageOptions: {
            ecmaVersion: 'latest',
            sourceType: 'module',
            globals: {
                ...globals.node,
                ...globals.browser,
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
            'no-unused-vars': ['error', {
                caughtErrors: 'none',
                argsIgnorePattern: '^_',
                varsIgnorePattern: '^_',
                destructuredArrayIgnorePattern: '^_',
            }],
            'no-param-reassign': ['error', {
                props: true,
                // 触觉合成与会话记录按设计原地写复用对象；名单内的形参属性可写。
                ignorePropertyModificationsFor: ['args', 'record', 'session', 'state', 'gate', 'cursor', 'phases'],
            }],

            // ------- 关闭的规则 -------
            '@stylistic/max-len': 'off',
            'max-len': 'off',
            'no-unsafe-optional-chaining': 'off',

            // ------- Vue -------
            'vue/multi-word-component-names': 'off',
            'vue/html-button-has-type': 'off',
            'vue/max-len': 'off',
            'vue/max-attributes-per-line': ['error', {
                singleline: { max: 5 },
                multiline: { max: 1 },
            }],
        },
    },
];

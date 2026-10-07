// pc/gui 前端样式的 stylelint 规则：套用 du 项目的规则并适配纯 CSS（无 SCSS），
// Vue SFC 内的 <style> 经 postcss-html 解析。
/** @type {import('stylelint').Config} */
export default {
    ignoreFiles: ['**/node_modules/**', '**/dist/**'],
    extends: [
        'stylelint-config-standard',
        'stylelint-config-recommended-vue',
    ],
    plugins: [
        '@stylistic/stylelint-plugin',
    ],
    rules: {
        '@stylistic/indentation': 2,
        'rule-empty-line-before': [
            'always',
            {
                except: ['first-nested', 'after-single-line-comment'],
            },
        ],
        '@stylistic/selector-list-comma-newline-after': 'always',
        '@stylistic/selector-list-comma-newline-before': 'never-multi-line',
        '@stylistic/selector-list-comma-space-before': 'never',
        '@stylistic/value-list-comma-space-after': 'always',
        'declaration-empty-line-before': ['never', {}],
        '@stylistic/color-hex-case': 'lower',
        'length-zero-no-unit': true,
        'color-hex-length': 'short',
        'number-max-precision': null,
        'comment-whitespace-inside': 'always',
        '@stylistic/string-quotes': 'single',
        '@stylistic/declaration-colon-space-after': 'always',
        '@stylistic/declaration-colon-space-before': 'never',
        '@stylistic/property-case': 'lower',
        'selector-type-case': 'lower',
        '@stylistic/media-feature-parentheses-space-inside': 'never',
        '@stylistic/block-opening-brace-space-before': 'always',
        '@stylistic/block-opening-brace-newline-after': 'always-multi-line',
        '@stylistic/block-closing-brace-newline-before': 'always-multi-line',
        '@stylistic/block-closing-brace-empty-line-before': 'never',
        '@stylistic/no-extra-semicolons': true,
        '@stylistic/number-leading-zero': 'never',
        '@stylistic/no-empty-first-line': true,
        'no-empty-source': null,
        '@stylistic/no-eol-whitespace': true,
        '@stylistic/no-missing-end-of-source-newline': true,
        '@stylistic/max-empty-lines': 1,
        '@stylistic/declaration-block-semicolon-newline-after': 'always-multi-line',
        '@stylistic/declaration-block-semicolon-space-after': 'always-single-line',
        '@stylistic/declaration-block-semicolon-space-before': 'never',
        '@stylistic/selector-combinator-space-before': 'always',
        '@stylistic/selector-combinator-space-after': 'always',

        // 基础覆盖设定
        'selector-class-pattern': null,
        'no-descending-specificity': null,
        'selector-not-notation': 'simple',
        'declaration-property-value-no-unknown': null,
        // tailwind 与 mde-vue 的样式入口约定用字符串 @import。
        'import-notation': 'string',
        // Material Symbols 是图标字体，没有衬线兜底族。
        'font-family-no-missing-generic-family-keyword': [true, { ignoreFontFamilies: ['Material Symbols Outlined'] }],
    },
    overrides: [
        {
            files: ['**/*.vue'],
            customSyntax: 'postcss-html',
        },
    ],
};

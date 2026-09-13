/**
 * 页面底部垫高：悬浮底栏（h-64 + 上下各 8 边距）盖在所有页面之上，
 * 垫高不由每页末尾的占位节点承担，而是统一写进滚动列的底部内边距（theme 的
 * scrollColumn / scrollColumnGap4 / scrollColumnCenter，值就是本常量），
 * 页面各自的 contentH 估算引用同一数值：少掉每页一个节点（按需挂载下每节点
 * 约 50 ms 建树成本，见 docs/adr/0014-page-mount-on-demand-progressive-fill.md），
 * 各页也不必再各写一遍底部留白。
 *
 * 高度在底栏遮挡范围（64 + 8 = 72）之外再留一段兜底：页面内容高度是静态
 * 估算值，卡片内边距、行数与条件行都可能带来偏差，这段余量保证滚到底时
 * 最后一张卡片仍完整露在底栏上方。
 */
/** 垫高高度 = 底栏遮挡 72 + 兜底 48。 */
export const BOTTOM_PAD_H = 120;

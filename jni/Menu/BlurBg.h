#pragma once
// 游戏内高斯模糊：从后缓冲抓面板下方的画面，两趟高斯模糊后塞进 ImGui 背景层
namespace BlurBg {
    // 每帧在 ImGui::NewFrame() 之后、任何自绘之前调用
    // enabled=false 或尺寸无效时什么都不做
    // 每帧 UI 绘制之前（游戏画面还在后缓冲时）抓一次区域，避免反射到上一帧 UI
    void capture(float dispW, float dispH, float x, float y, float w, float h);
    // x/y/w/h = 贴图要摆放的矩形（=抓帧矩形，保证不拉伸）；cx..ch = 裁剪矩形（=当前窗口）
    void draw(float dispW, float dispH, float x, float y, float w, float h,
              float cx, float cy, float cw, float ch,
              float rounding, int strength, bool enabled);
    void shutdown();
    // GL 上下文重建后调用：丢弃所有 GL 对象句柄，下次 capture 重新创建
    void onContextReset();
}

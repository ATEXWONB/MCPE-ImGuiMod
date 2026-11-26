// ============================================================
//  include/animation/animation.h  —  窗口动画 & 灵动岛引擎
//  作用：丝滑形变过渡 + 灵动岛迷你模式 + 红绿灯三点
//  用法：draw_Gui.cpp 包含此头文件，创建 IslandContext 实例
//  触摸判定：点击位置到圆点/灵动岛中心距离 <= 阈值即触发
// ============================================================

#pragma once
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include <cmath>

// ---------- 辅助：计算两点之间的距离 ----------
inline float dist2D(float x1, float y1, float x2, float y2) {
    float dx = x1 - x2;
    float dy = y1 - y2;
    return std::sqrt(dx * dx + dy * dy);
}

// ---------- 1. 动画插值引擎 ----------
// 每帧平滑逼近目标值（指数衰减，与帧率无关）
// curr   : 当前值（引用，会被修改）
// target : 目标值
// speed  : 过渡速度，越大越快，建议 10~20
// dt     : 帧间隔，取 ImGui::GetIO().DeltaTime
inline void Anim_Float(float& curr, float target, float speed, float dt) {
    curr += (target - curr) * speed * dt;
    if (fabsf(curr - target) < 0.15f) {
        curr = target;  // 足够近时直接到位，避免永远差一点点
    }
}

// ---------- 2. 窗口形态枚举 ----------
enum AnimState {
    STATE_MINIMIZED,  // 灵动岛迷你模式（顶部小胶囊）
    STATE_FULL        // 完整窗口模式
};

// ---------- 3. 灵动岛状态存储 ----------
// 每帧由调用方传入并维护，全部状态保存在此结构体中
struct IslandContext {
    // ---- 状态机 ----
    AnimState state         = STATE_FULL;   // 当前形态
    bool      transitioning = false;        // 是否正在播放过渡动画

    // ---- 动画实时值（动画期间每帧更新） ----
    float curX = 100, curY = 150;           // 窗口当前坐标
    float curW = 360, curH = 500;           // 窗口当前宽高
    float contentAlpha = 1.0f;              // 内容透明度（最小化时淡出）

    // ---- 用户拖拽记忆（完整模式下记录，最小化后恢复） ----
    float userX = -1, userY = 120;          // 用户最后拖拽到的位置
    float userW = 360, userH = 540;         // 用户最后缩放到的尺寸

    // ---- 灵动岛尺寸（可动态按文字宽度调整，适配大屏） ----
    static constexpr float islandW = 260;   // 参考值
    static constexpr float islandH = 48;    // 参考值
    static constexpr float islandY = 35;    // 参考值
    float miniW = 620.f;                    // 实际使用的胶囊宽
    float miniH = 132.f;                    // 实际使用的胶囊高
    float miniY = 34.f;                     // 胶囊距顶距离

    // ---- 动画速度 ----
    static constexpr float speed = 14.0f;   // 窗口形变速度（越大越快）

    // ---- 触摸判定距离阈值 ----
    static constexpr float kTapThreshold = 50.0f;   // 三点/灵动岛点击判定半径
};

// ---------- 4. 窗口位置/大小动画更新 ----------
// 每帧在 ImGui::Begin 之前调用一次
// 内部会根据状态调用 SetNextWindowPos / SetNextWindowSize
// 返回 true 表示动画正在播放中
inline bool Island_UpdateTarget(IslandContext& ctx, float screenW, float screenH, float dt) {
    // ---- 计算目标值 ----
    float tx, ty, tw, th, ta;
    if (ctx.state == STATE_MINIMIZED) {
        tw = ctx.miniW;
        th = ctx.miniH;
        tx = (screenW - tw) * 0.5f;
        ty = ctx.miniY;
        ta = 0.0f;  // 灵动岛模式：内容全透明
    } else {
        tw = ctx.userW;
        th = ctx.userH;
        tx = ctx.userX;
        ty = ctx.userY;
        ta = 1.0f;  // 完整模式：内容全显示
    }

    // ---- 动画中：逐帧插值 ----
    if (ctx.transitioning) {
        Anim_Float(ctx.curX, tx, ctx.speed, dt);
        Anim_Float(ctx.curY, ty, ctx.speed, dt);
        Anim_Float(ctx.curW, tw, ctx.speed, dt);
        Anim_Float(ctx.curH, th, ctx.speed, dt);
        Anim_Float(ctx.contentAlpha, ta, 15.0f, dt);

        ImGui::SetNextWindowPos(ImVec2(ctx.curX, ctx.curY));
        ImGui::SetNextWindowSize(ImVec2(ctx.curW, ctx.curH));

        // 判定动画结束：位置和尺寸都接近目标值
        if (fabsf(ctx.curW - tw) < 0.5f &&
            fabsf(ctx.curH - th) < 0.5f &&
            fabsf(ctx.curX - tx) < 0.5f) {
            ctx.transitioning = false;
        }
        return true;
    }

    // ---- 稳态：直接定位 ----
    if (ctx.state == STATE_MINIMIZED) {
        ImGui::SetNextWindowPos(ImVec2(tx, ty));
        ImGui::SetNextWindowSize(ImVec2(tw, th));
    } else {
        ImGui::SetNextWindowPos(ImVec2(ctx.userX, ctx.userY), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(ctx.userW, ctx.userH), ImGuiCond_Once);
    }
    return false;
}

// ---------- 5. 灵动岛迷你栏绘制 ----------
// 仅在 STATE_MINIMIZED 模式下调用
// 绘制可点击的胶囊条 + 跑马灯文字
// 使用距离判定：用户触摸位置到灵动岛中心 <= kTapThreshold 即触发展开
inline bool Island_DrawMiniBar(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* text, float time) {
    // ---- 先检测鼠标点击（距离判定） ----
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(0)) {
        float cx = pos.x + size.x * 0.5f;   // 灵动岛中心 X
        float cy = pos.y + size.y * 0.5f;   // 灵动岛中心 Y
        if (dist2D(io.MousePos.x, io.MousePos.y, cx, cy) <= IslandContext::kTapThreshold) {
            return true;  // 点击在阈值内 → 展开
        }
    }

    // ---- 文字居中 ----
    ImVec2 textPos(pos.x + 16, pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f);
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float  maxWidth = size.x - 30;

    // ---- 裁剪区域（防止文字溢出灵动岛圆角边界） ----
    dl->PushClipRect(ImVec2(pos.x + 12, pos.y), ImVec2(pos.x + size.x - 12, pos.y + size.y), true);

    if (textSize.x > maxWidth) {
        // 文字过长 → 跑马灯无限循环滚动
        float loop   = textSize.x + 50;
        float offset = fmodf(time * 45.0f, loop);
        dl->AddText(ImVec2(textPos.x - offset, textPos.y), IM_COL32(0, 255, 230, 255), text);
        dl->AddText(ImVec2(textPos.x - offset + loop, textPos.y), IM_COL32(0, 255, 230, 255), text);
    } else {
        dl->AddText(textPos, IM_COL32(0, 255, 230, 255), text);
    }

    dl->PopClipRect();
    return false;
}

// ---------- 6. 红绿灯三点（最小化按钮） ----------
// 在完整模式下调用，绘制左上角三个圆点
// 使用距离判定：用户触摸位置到任意圆点中心 <= kTapThreshold 即触发最小化
inline bool Island_DrawTrafficLights(ImVec2 winPos) {
    const float visR = 8.5f;   // 视觉圆点半径

    // 三个圆点中心坐标
    float cx[3] = { winPos.x + 25, winPos.x + 48, winPos.x + 71 };
    float cy    = winPos.y + 25;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 绘制三个彩色圆点（红、黄、绿）
    dl->AddCircleFilled(ImVec2(cx[0], cy), visR, IM_COL32(255, 95, 87, 255));   // 红
    dl->AddCircleFilled(ImVec2(cx[1], cy), visR, IM_COL32(255, 189, 46, 255));  // 黄
    dl->AddCircleFilled(ImVec2(cx[2], cy), visR, IM_COL32(39, 201, 63, 255));   // 绿

    // ---- 距离判定：鼠标点击是否落在阈值范围内 ----
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(0)) {
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        // 遍历三个圆点，任意一个距离 <= 阈值即触发
        for (int i = 0; i < 3; i++) {
            if (dist2D(mx, my, cx[i], cy) <= IslandContext::kTapThreshold) {
                return true;  // 点击在阈值内 → 最小化
            }
        }
    }

    return false;
}
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "openxr_context.h"
#include "vr.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace afvr
{
    // One XrAPI tracking packet (WinlatorXR -> game, UDP 127.0.0.1:7872).
    // Poses use OpenXR conventions: +X right, +Y up, -Z forward, meters.
    struct WinlatorXrPacket
    {
        bool valid = false;
        std::array<XrPosef, 2> hands{};       // controller aim poses
        std::array<XrVector2f, 2> thumbsticks{};
        XrPosef head{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        float ipd = 0.064f;
        float fov_h_deg = 0.0f;
        float fov_v_deg = 0.0f;
        int sync = 0;                         // HMD_SYNC, echoed in the frame stamp
        float hmd_altitude = 0.0f;            // protocol 0.5+
        // Wire order: L grip, L menu, L stick, L stick left/right/up/down,
        // L trigger, X, Y, A, B, R grip, R stick, R stick left/right/up/down,
        // R trigger.
        std::array<bool, 19> buttons{};
        uint64_t sequence = 0;                // increments per received packet
    };

    enum WinlatorXrButton
    {
        wxr_left_grip = 0,
        wxr_left_menu = 1,
        wxr_left_thumbstick = 2,
        wxr_left_trigger = 7,
        wxr_left_x = 8,
        wxr_left_y = 9,
        wxr_right_a = 10,
        wxr_right_b = 11,
        wxr_right_grip = 12,
        wxr_right_thumbstick = 13,
        wxr_right_trigger = 18,
    };

    [[nodiscard]] bool parse_winlatorxr_packet(char* text, WinlatorXrPacket& packet);

    // True when the game runs inside a WinlatorXR container with XrAPI enabled
    // (Z:\tmp\xr\system exists). AFVR_RUNTIME=openxr|winlatorxr overrides it.
    [[nodiscard]] bool winlatorxr_runtime_detected();

    // Process setup that must happen before the D3D11 device/swap chain exist.
    void winlatorxr_prepare_process();

    class WinlatorXrContext final : public OpenXrContext
    {
    public:
        WinlatorXrContext() = default;
        ~WinlatorXrContext() override;

        [[nodiscard]] bool initialize(ID3D11Device* device) override;
        [[nodiscard]] bool poll_events() override;
        [[nodiscard]] bool sync_actions() override;
        [[nodiscard]] bool wait_frame() override;
        [[nodiscard]] bool render_frame(const OpenXrEyeRenderCallback& render_eye,
            const OpenXrHudRenderCallback& render_hud = {}) override;
        [[nodiscard]] bool render_menu_frame(const OpenXrMenuRenderCallback& render_menu) override;
        [[nodiscard]] bool render_scope_frame(const OpenXrMenuRenderCallback& render_scope) override;
        void set_menu_active(bool active) override;
        void set_scope_active(bool active) override;
        void set_hud_active(bool active) override;
        void reset_fixed_quad_poses() override;
        [[nodiscard]] bool get_menu_pointer(float& u, float& v) const override;
        void shutdown() override;
        [[nodiscard]] bool is_initialized() const override { return initialized_; }
        [[nodiscard]] bool is_winlatorxr() const override { return true; }

        [[nodiscard]] bool take_compose_info(WinlatorXrComposeInfo& info);
        void frame_presented(bool stereo);
        [[nodiscard]] bool block_desktop_keyboard() const { return settings_.block_keyboard; }
        [[nodiscard]] bool show_crosshair() const { return settings_.crosshair; }

    private:
        struct Settings
        {
            int eye_height = 0;              // 0 = back buffer height
            float fov_scale = 1.0f;
            float hud_distance_m = 2.4f;
            float hud_width_m = 5.0f;        // width of the whole (uncropped) HUD image
            float hud_crop = 0.22f;          // part of each HUD side shown (0.5 = all)
            float hud_gap_m = 0.1f;          // gap between the left and right HUD strips
            float hud_height_m = 0.0f;       // vertical offset of the HUD (+ up)
            float grip_pitch_deg = 0.0f;     // grip orientation relative to aim
            int flip_controllers = -1;       // -1 auto (Quest 2/Pro/Pico), 0, 1
            int sync_wait_ms = 20;
            bool block_keyboard = true;
            // Mobile GPU graphics overrides (-1 / negative = keep the game setting).
            int shadow_quality = 0;          // 0 = blob shadows, no shadow map
            int mesh_lighting = 1;           // 1 = vertex lighting instead of per-pixel
            float lod_scale = 1.0f;          // stock RF LOD distances
            int big_hud = 1;                 // Alpine big HUD (readable on the headset)
            bool crosshair = false;          // aim dot where the weapon points
            long long cpu_affinity = -1;     // -1 fastest cores, 0 unpinned, >0 mask
            bool menu_2d = true;             // pause/main menu on WinlatorXR's sharp 2D screen
            float screen_distance_m = 1.6f;  // menu / scope virtual screen distance
            float screen_width_m = 2.4f;     // menu / scope virtual screen width
        };

        struct EyeTarget
        {
            int width = 0;
            int height = 0;
            ComPtr<ID3D11Texture2D> color;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11Texture2D> depth;
            ComPtr<ID3D11DepthStencilView> dsv;
        };

        void receive_loop();
        void send_state(int mode_vr, int mode_3d, float fov_x, float fov_y);
        void write_marker_files();
        void read_system_info();
        void reload_settings(bool force);
        void pin_game_thread_to_fast_cores();
        void ensure_window();
        [[nodiscard]] bool consume_packet(bool wait);
        [[nodiscard]] bool ensure_eye_targets();
        [[nodiscard]] bool ensure_hud_target();
        [[nodiscard]] bool ensure_screen_target();
        [[nodiscard]] bool render_screen_frame(const OpenXrMenuRenderCallback& render);
        [[nodiscard]] bool screen_ray_uv(const XrPosef& aim, float& u, float& v) const;
        [[nodiscard]] XrPosef current_center_pose() const;
        [[nodiscard]] XrPosef aim_pose(const XrPosef& raw) const;
        [[nodiscard]] XrPosef grip_pose(const XrPosef& aim) const;

        bool initialized_ = false;
        ID3D11Device* device_ = nullptr;
        Settings settings_{};
        std::chrono::steady_clock::time_point settings_checked_{};
        long long settings_write_time_ = -1;
        bool flip_controllers_auto_ = false;

        // UDP receiver
        std::thread receiver_;
        std::atomic<bool> receiver_running_{false};
        uintptr_t receive_socket_ = ~uintptr_t{0};
        uintptr_t send_socket_ = ~uintptr_t{0};
        bool wsa_started_ = false;
        mutable std::mutex packet_mutex_;
        std::condition_variable packet_cv_;
        WinlatorXrPacket latest_packet_{};
        int receive_port_ = 0;
        std::atomic<uint32_t> datagrams_received_{0};
        std::atomic<uint32_t> datagrams_rejected_{0};
        std::string rejected_example_;
        bool rejected_example_logged_ = false;

        // Per-frame state (main thread)
        WinlatorXrPacket frame_packet_{};
        uint64_t consumed_sequence_ = 0;
        float native_fov_h_deg_ = 0.0f;
        float native_fov_v_deg_ = 0.0f;
        float tan_half_h_ = 1.0f;
        float tan_half_v_ = 1.0f;
        std::array<EyeTarget, 2> eyes_{};
        EyeTarget hud_{};
        EyeTarget screen_{};
        bool screen_anchor_valid_ = false;
        XrPosef screen_pose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        bool menu_active_ = false;
        bool scope_active_ = false;
        bool hud_active_ = false;
        bool compose_pending_ = false;
        WinlatorXrComposeInfo compose_info_{};
        int last_mode_vr_ = -1;
        int last_mode_3d_ = -1;

        // Diagnostics
        bool first_packet_logged_ = false;
        bool poll_logged_ = false;
        bool first_hud_logged_ = false;
        bool first_stereo_logged_ = false;
        bool packet_timeout_logged_ = false;
        uint32_t last_window_datagrams_ = 0;
        uint32_t presented_frames_ = 0;
        uint32_t stereo_frames_ = 0;
        std::chrono::steady_clock::time_point fps_window_start_{};
    };
}

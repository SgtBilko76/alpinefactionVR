// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace rf
{
    struct Matrix3;
    struct Vector3;
}

struct ID3D11ShaderResourceView;

namespace afvr
{
    enum class TimingPhase
    {
        input_sync,
        begin_frame,
        eye_image_wait,
        eye_render,
        eye_release,
        hud_image_wait,
        hud_render,
        hud_release,
        menu_image_wait,
        menu_copy,
        menu_release,
        end_frame,
        count,
    };

    // Register -vr before Red Faction parses its command line.
    void register_command_line();

    // Intercept the local portal/world traversal for stereo rendering.
    void install_render_hook();

    // Register developer VR console commands after AlpineFaction has replaced
    // Red Faction's fixed-size command buffer.
    void register_console_commands();

    // Low-frequency end-to-end diagnostics used to identify whether XR pacing,
    // the RF frame loop, rendering work, or the desktop mirror limits VR.
    void timing_game_frame_begin();
    void timing_game_frame_end();
    void timing_note_xr_wait(double wait_ms, double return_interval_ms,
        double predicted_interval_ms, double runtime_target_hz);
    void timing_note_phase(TimingPhase phase, double duration_ms);
    void timing_note_xr_submission();
    void timing_note_desktop_present(double duration_ms);

    // Called after the original game initialization, when D3D11 is available.
    void after_game_init();

    // Poll local OpenXR runtime/session events once per game frame.
    void update();

    // Copy the completed native RF menu into a spatially anchored OpenXR quad.
    // Called by the existing menu renderer after it has drawn its cursor/widgets.
    void submit_menu_frame();

    // Copy the completed native zoom/scope frame into a head-following OpenXR
    // quad. Precision- and sniper-rifle scopes use this instead of stereo world
    // rendering until alternate fire closes the scope.
    void submit_scope_frame();

    // Release all local OpenXR state. Safe to call more than once.
    void shutdown();

    [[nodiscard]] bool is_requested();
    [[nodiscard]] bool is_initialized();
    [[nodiscard]] bool is_session_running();
    [[nodiscard]] bool is_menu_capture_active();
    [[nodiscard]] bool is_scope_capture_active();
    [[nodiscard]] bool should_update_desktop_mirror();

    // Physical mouse input remains available in RF's main menu, but must not
    // affect gameplay or in-game menus while an OpenXR session is active.
    // VR controller menu input is injected separately and is not blocked.
    [[nodiscard]] bool should_block_physical_mouse_input();

    // Re-neutralize HMD yaw and tracking-space position on the next valid XR
    // view without modifying Red Faction's player/body orientation.
    void recenter_tracking();

    // One-shot VR render coverage diagnostics. Passes: 0 left, 1 right.
    // These count draw calls, never simulation updates.
    void begin_scene_render_pass(int pass);
    void end_scene_render_pass(int pass);
    void note_static_solid_draw();
    void note_movable_solid_draw();
    void note_standard_mesh_draw();
    void note_character_mesh_draw();

    // True only while the local first-person weapon is being submitted to an
    // OpenXR eye. The normal desktop viewmodel remains suppressed in VR mode.
    [[nodiscard]] bool is_rendering_weapon();

    // D3D11 FPGUN material filter. RF arms/hands share the animated VMesh but
    // use dedicated material chunks, allowing VR to keep the moving weapon.
    [[nodiscard]] bool should_render_fpgun_texture(int bitmap_handle);

    // Gameplay fire point synchronized to the resolved visual laser emitter.
    // Position and orientation match the visible beam exactly for every gun.
    [[nodiscard]] bool get_weapon_muzzle_pose(rf::Vector3& position, rf::Matrix3& orientation);

    // Final local on-foot projectile-creation pose. Guns, grenades, and remote
    // charges use the synchronized laser/fire position and orientation.
    [[nodiscard]] bool get_weapon_launch_pose(int weapon_type,
        rf::Vector3& position, rf::Matrix3& orientation);

    // Latest local right-controller grip position with the controller aim
    // orientation, used by first-person effects that must follow the hand but
    // are not emitted from the weapon muzzle.
    [[nodiscard]] bool get_right_controller_pose(rf::Vector3& position,
        rf::Matrix3& orientation);

    // Latest center-HMD pose in RF world space. This is cached from the XR view
    // submission for local launch paths that intentionally follow
    // the player's six-degree look direction rather than body/stick yaw.
    [[nodiscard]] bool get_head_pose(rf::Vector3& position, rf::Matrix3& orientation);

    // Local player's resolved visual body pose. Gameplay/collision continues
    // to use the native entity transform; this pose is for character rendering
    // (including mirrors, third person, attachments, and shadows).
    [[nodiscard]] bool get_visual_body_pose(rf::Vector3& position,
        rf::Matrix3& orientation);
    [[nodiscard]] bool is_primary_trigger_active();

    // WinlatorXR (XrAPI over UDP) backend for standalone headsets running the
    // game under Wine. There is no compositor: both eyes are composed side by
    // side into the desktop back buffer, which WinlatorXR shows in the HMD.
    [[nodiscard]] bool is_winlatorxr_active();

    struct WinlatorXrComposeInfo
    {
        ID3D11ShaderResourceView* eye_views[2] = {nullptr, nullptr};
        ID3D11ShaderResourceView* hud_view = nullptr;
        // The HUD is drawn as two strips: the left part of the HUD image
        // (u 0..hud_crop) and the right part (u 1-hud_crop..1), each placed in
        // its own rectangle so the empty middle of the HUD is dropped.
        // Rectangles are in eye NDC (-1..1, +y up): left, right, bottom, top.
        float hud_crop = 0.5f;
        float hud_rect[2][2][4] = {}; // [eye][strip][edge]
        // True when hud_view has a mip chain that must be regenerated.
        bool hud_generate_mips = false;
        // Menus and the native rifle scope: the flat game image shown on a
        // virtual screen fixed in the room. Corners are clip-space positions
        // (x, y, z, w) per eye in triangle-strip order TL, TR, BL, BR.
        ID3D11ShaderResourceView* screen_view = nullptr;
        float screen_clip[2][4][4] = {};
        // XrAPI HMD_SYNC value of the pose this frame was rendered with.
        int sync_value = 0;
    };

    // Returns true when this RF frame rendered a stereo world frame that must
    // be composed into the back buffer before Present.
    [[nodiscard]] bool get_winlatorxr_compose_info(WinlatorXrComposeInfo& info);

    // Called once per presented frame. Reports the presented mode back to
    // WinlatorXR (stereo projection vs flat virtual screen).
    void winlatorxr_frame_presented(bool stereo);

    // GPU stall guard: a Present blocking for seconds means the headset GPU is
    // hanging, which reboots the Quest within a minute. Repeated stalls turn
    // the headlamp off and stop sending dynamic lights to the pixel shader.
    void winlatorxr_note_present_ms(double ms);
    [[nodiscard]] bool winlatorxr_dynamic_lights_disabled();

    // WinlatorXR synthesizes desktop keyboard events from controller buttons.
    // AFVR injects all VR actions itself, so those events are dropped.
    [[nodiscard]] bool should_block_desktop_keyboard_input();
}

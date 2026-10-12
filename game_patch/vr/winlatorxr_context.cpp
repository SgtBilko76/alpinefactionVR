// SPDX-License-Identifier: MPL-2.0
//
// WinlatorXR backend for AlpineFaction VR.
//
// WinlatorXR runs Windows games under
// Wine/Box64 on standalone Android headsets. It exposes no OpenXR runtime;
// instead its XrAPI streams head/controller tracking as UDP text to
// 127.0.0.1:7872 and accepts "L_HAPTIC R_HAPTIC MODE_VR MODE_3D FOV_X FOV_Y" on
// 127.0.0.1:7278. The game draws both eyes side by side into its window and
// stamps the HMD_SYNC value of the rendered pose into screen pixel (0,0).
//
// The class implements the OpenXrContext interface so vr.cpp's tracking,
// rendering and input code runs unchanged; only frame composition differs.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "winlatorxr_context.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <xlog/xlog.h>
#include "../rf/gr/gr.h"
#include "../rf/os/os.h"
#include "../misc/alpine_settings.h"

namespace afvr
{
    namespace
    {
        constexpr u_short listen_port = 7872;
        constexpr u_short listen_port_fallback = 7873;
        constexpr u_short send_port = 7278;
        constexpr const char* xr_dir = "Z:\\tmp\\xr";
        constexpr const char* settings_file_name = "afvr_winlatorxr.ini";

        SOCKET as_socket(uintptr_t value)
        {
            return static_cast<SOCKET>(value);
        }

        XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v)
        {
            const XrVector3f u{q.x, q.y, q.z};
            const XrVector3f t{
                2.0f * (u.y * v.z - u.z * v.y),
                2.0f * (u.z * v.x - u.x * v.z),
                2.0f * (u.x * v.y - u.y * v.x),
            };
            return {
                v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
                v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
            };
        }

        XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b)
        {
            return {
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            };
        }

        bool normalize(XrQuaternionf& q)
        {
            const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (length < 1.0e-4f || !std::isfinite(length)) {
                return false;
            }
            q.x /= length;
            q.y /= length;
            q.z /= length;
            q.w /= length;
            return true;
        }

        std::string game_directory()
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            std::string result{path};
            const auto slash = result.find_last_of("\\/");
            return slash == std::string::npos ? std::string{} : result.substr(0, slash + 1);
        }

        std::string upper(std::string value)
        {
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return value;
        }
    }

    bool parse_winlatorxr_packet(char* text, WinlatorXrPacket& packet)
    {
        char* context = nullptr;
        char* token = strtok_s(text, " \r\n", &context);
        if (!token) {
            return false; // client id, unused
        }

        std::array<float, 28> values{};
        for (float& value : values) {
            token = strtok_s(nullptr, " \r\n", &context);
            if (!token) {
                return false;
            }
            value = static_cast<float>(std::atof(token));
        }

        token = strtok_s(nullptr, " \r\n", &context);
        if (!token) {
            return false;
        }
        packet.sync = std::clamp(std::atoi(token), 0, 255);

        token = strtok_s(nullptr, " \r\n", &context);
        if (!token || std::strlen(token) < packet.buttons.size()) {
            return false;
        }
        for (size_t i = 0; i < packet.buttons.size(); ++i) {
            packet.buttons[i] = token[i] == 'T';
        }

        for (size_t hand = 0; hand < 2; ++hand) {
            const size_t base = hand * 9;
            packet.hands[hand].orientation = {
                values[base + 0], values[base + 1], values[base + 2], values[base + 3]};
            packet.thumbsticks[hand] = {values[base + 4], values[base + 5]};
            packet.hands[hand].position = {
                values[base + 6], values[base + 7], values[base + 8]};
        }
        packet.head.orientation = {values[18], values[19], values[20], values[21]};
        packet.head.position = {values[22], values[23], values[24]};
        packet.ipd = values[25];
        packet.fov_h_deg = values[26];
        packet.fov_v_deg = values[27];

        // Protocol 0.5 appends HMD_ALTITUDE and the (non-OpenXR) grip
        // orientations; a trailing flags token is not numeric.
        token = strtok_s(nullptr, " \r\n", &context);
        if (token) {
            char* end = nullptr;
            const float altitude = std::strtof(token, &end);
            if (end != token && *end == '\0') {
                packet.hmd_altitude = altitude;
            }
        }

        if (!normalize(packet.head.orientation)) {
            return false;
        }
        packet.valid = true;
        return true;
    }

    bool winlatorxr_runtime_detected()
    {
        if (const char* runtime = std::getenv("AFVR_RUNTIME")) {
            const std::string value = upper(runtime);
            if (value == "OPENXR") {
                return false;
            }
            if (value == "WINLATORXR") {
                return true;
            }
        }
        const DWORD attributes = GetFileAttributesA("Z:\\tmp\\xr\\system");
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    void winlatorxr_prepare_process()
    {
        // WinlatorXR starts games with DXVK_FRAME_RATE=72. Under Box64 that
        // limiter's coarse sleeps quantize the game to 36/24 fps as soon as a
        // frame misses the refresh. Pacing comes from the HMD_SYNC wait instead.
        SetEnvironmentVariableA("DXVK_FRAME_RATE", "0");
    }

    WinlatorXrContext::~WinlatorXrContext()
    {
        WinlatorXrContext::shutdown();
    }

    // Red Faction runs its whole frame on one thread, and under Box64 that
    // thread is the frame-rate limit. On a Quest 3 two of the six cores run
    // at a lower maximum clock, so keep the game thread on the fastest ones
    // (clocks from Linux sysfs; Wine reports every core as equal).
    // cpu_affinity in the ini: -1 auto, 0 leave unpinned, >0 explicit mask.
    void WinlatorXrContext::pin_game_thread_to_fast_cores()
    {
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
        DWORD_PTR mask = 0;
        std::string reason;
        if (settings_.cpu_affinity == 0) {
            xlog::info("[AFVR][WinlatorXR] Game thread left unpinned (cpu_affinity=0)");
            return;
        }
        if (settings_.cpu_affinity > 0) {
            mask = static_cast<DWORD_PTR>(settings_.cpu_affinity) & process_mask;
            reason = "cpu_affinity from the ini";
        }
        else {
            unsigned long long fastest = 0;
            std::string clocks;
            for (unsigned cpu = 0; cpu < sizeof(DWORD_PTR) * 8; ++cpu) {
                const DWORD_PTR bit = static_cast<DWORD_PTR>(1) << cpu;
                if (!(process_mask & bit)) {
                    continue;
                }
                std::ifstream file{std::format(
                    "\\\\?\\unix\\sys\\devices\\system\\cpu\\cpu{}\\cpufreq\\cpuinfo_max_freq", cpu)};
                unsigned long long khz = 0;
                if (!(file >> khz)) {
                    continue;
                }
                clocks += std::format("{}{}:{}", clocks.empty() ? "" : ", ", cpu, khz / 1000);
                if (khz > fastest) {
                    fastest = khz;
                    mask = 0;
                }
                if (khz == fastest) {
                    mask |= bit;
                }
            }
            reason = std::format("fastest cores (cpu:MHz {})", clocks);
            if (!mask || mask == process_mask) {
                xlog::info("[AFVR][WinlatorXR] Game thread left unpinned; {}", clocks.empty() ? "no CPU clocks" : reason);
                return;
            }
        }
        if (!mask) {
            return;
        }
        const DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), mask);
        xlog::info("[AFVR][WinlatorXR] Game thread pinned to CPU mask 0x{:x}, {}{}",
            static_cast<unsigned long long>(mask), reason, previous ? "" : " (SetThreadAffinityMask failed)");
    }

    bool WinlatorXrContext::initialize(ID3D11Device* device)
    {
        WinlatorXrContext::shutdown();
        if (!device) {
            xlog::error("[AFVR][WinlatorXR] D3D11 device is null");
            return false;
        }
        device_ = device;
        xlog::info("[AFVR][WinlatorXR] Initializing XrAPI backend (no OpenXR runtime in this container)");

        reload_settings(true);
        read_system_info();
        pin_game_thread_to_fast_cores();

        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            xlog::error("[AFVR][WinlatorXR] WSAStartup failed");
            return false;
        }
        wsa_started_ = true;

        for (u_short port : {listen_port, listen_port_fallback}) {
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) {
                continue;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(port);
            if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                DWORD timeout_ms = 200;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
                receive_socket_ = static_cast<uintptr_t>(sock);
                receive_port_ = port;
                break;
            }
            xlog::warn("[AFVR][WinlatorXR] Could not bind UDP port {} (error {})",
                port, WSAGetLastError());
            closesocket(sock);
        }
        if (as_socket(receive_socket_) == INVALID_SOCKET) {
            xlog::error("[AFVR][WinlatorXR] No XrAPI UDP port could be bound");
            shutdown();
            return false;
        }

        SOCKET send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        send_socket_ = static_cast<uintptr_t>(send_sock);

        write_marker_files();

        receiver_running_ = true;
        receiver_ = std::thread{[this] { receive_loop(); }};

        // Announce ourselves in flat screen mode; stereo starts with the first
        // composed world frame.
        send_state(2, 0, 0.0f, 0.0f);
        initialized_ = true;
        fps_window_start_ = std::chrono::steady_clock::now();
        xlog::info("[AFVR][WinlatorXR] Listening for XrAPI tracking on UDP {}", receive_port_);
        return true;
    }

    void WinlatorXrContext::write_marker_files()
    {
        CreateDirectoryA("Z:\\tmp", nullptr);
        CreateDirectoryA(xr_dir, nullptr);
        // XrAPI 0.5: 0.2 wire format + HMD altitude + grip orientations.
        if (std::ofstream version{std::string{xr_dir} + "\\version", std::ios::trunc}) {
            version << "0.5";
        }
        else {
            xlog::warn("[AFVR][WinlatorXR] Could not write {}\\version", xr_dir);
        }
        if (std::ofstream vr{std::string{xr_dir} + "\\vr", std::ios::trunc}) {
            vr << "VR";
        }
        else {
            xlog::warn("[AFVR][WinlatorXR] Could not write {}\\vr", xr_dir);
        }
    }

    void WinlatorXrContext::read_system_info()
    {
        std::ifstream file{std::string{xr_dir} + "\\system"};
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        std::string joined;
        for (const auto& line : lines) {
            joined += joined.empty() ? line : " | " + line;
        }
        xlog::info("[AFVR][WinlatorXR] System: {}", joined.empty() ? "<unknown>" : joined);

        const std::string manufacturer = lines.size() > 0 ? upper(lines[0]) : std::string{};
        const std::string product = lines.size() > 1 ? upper(lines[1]) : std::string{};
        // Quest 2 / Quest Pro / Pico report controller rotation rolled by 180
        // degrees; Quest 3 and 3S do not (matches the WinlatorXR reference mods).
        flip_controllers_auto_ =
            manufacturer.find("PICO") != std::string::npos ||
            manufacturer.find("PLAY FOR DREAM") != std::string::npos ||
            product.find("HOLLYWOOD") != std::string::npos ||
            product.find("SEACLIFF") != std::string::npos ||
            product.find("QUEST 2") != std::string::npos ||
            product.find("QUEST PRO") != std::string::npos;
    }

    void WinlatorXrContext::reload_settings(bool force)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - settings_checked_ < std::chrono::seconds(1)) {
            return;
        }
        settings_checked_ = now;

        const std::string path = game_directory() + settings_file_name;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        long long write_time = 0;
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
            write_time = (static_cast<long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                data.ftLastWriteTime.dwLowDateTime;
        }
        if (!force && write_time == settings_write_time_) {
            return;
        }
        settings_write_time_ = write_time;

        Settings settings{};
        std::ifstream file{path};
        for (std::string line; std::getline(file, line);) {
            const auto comment = line.find_first_of("#;");
            if (comment != std::string::npos) {
                line.erase(comment);
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            auto trim = [](std::string value) {
                const auto first = value.find_first_not_of(" \t\r");
                const auto last = value.find_last_not_of(" \t\r");
                return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
            };
            const std::string key = trim(line.substr(0, equals));
            const std::string value = trim(line.substr(equals + 1));
            const float number = static_cast<float>(std::atof(value.c_str()));
            if (key == "eye_height") settings.eye_height = std::clamp(static_cast<int>(number), 0, 4096);
            else if (key == "fov_scale") settings.fov_scale = std::clamp(number, 0.5f, 1.5f);
            else if (key == "hud_distance_m") settings.hud_distance_m = std::clamp(number, 0.3f, 20.0f);
            else if (key == "hud_width_m") settings.hud_width_m = std::clamp(number, 0.1f, 20.0f);
            else if (key == "hud_crop") settings.hud_crop = std::clamp(number, 0.05f, 0.5f);
            else if (key == "hud_gap_m") settings.hud_gap_m = std::clamp(number, -5.0f, 20.0f);
            else if (key == "hud_height_m") settings.hud_height_m = std::clamp(number, -5.0f, 5.0f);
            else if (key == "grip_pitch_deg") settings.grip_pitch_deg = std::clamp(number, -180.0f, 180.0f);
            else if (key == "flip_controllers") settings.flip_controllers = std::clamp(static_cast<int>(number), -1, 1);
            else if (key == "sync_wait_ms") settings.sync_wait_ms = std::clamp(static_cast<int>(number), 0, 100);
            else if (key == "block_keyboard") settings.block_keyboard = number != 0.0f;
            else if (key == "shadow_quality") settings.shadow_quality = std::clamp(static_cast<int>(number), -1, 5);
            else if (key == "mesh_lighting") settings.mesh_lighting = std::clamp(static_cast<int>(number), -1, 2);
            else if (key == "lod_scale") settings.lod_scale = std::clamp(number, -1.0f, 1000.0f);
            else if (key == "big_hud") settings.big_hud = std::clamp(static_cast<int>(number), -1, 1);
            else if (key == "crosshair") settings.crosshair = number != 0.0f;
            else if (key == "screen_distance_m") settings.screen_distance_m = std::clamp(number, 0.3f, 20.0f);
            else if (key == "cpu_affinity") settings.cpu_affinity = static_cast<long long>(std::strtoll(value.c_str(), nullptr, 0));
            else if (key == "menu_2d") settings.menu_2d = number != 0.0f;
            else if (key == "screen_width_m") settings.screen_width_m = std::clamp(number, 0.2f, 20.0f);
        }
        settings_ = settings;

        // The Quest GPU cannot afford Alpine's desktop defaults (4096 shadow
        // map, per-pixel mesh lighting, 10x LOD distance) for two eyes.
        if (settings_.shadow_quality >= 0) {
            g_alpine_game_config.set_shadow_quality(settings_.shadow_quality);
        }
        if (settings_.mesh_lighting >= 0) {
            g_alpine_game_config.mesh_lighting_mode = settings_.mesh_lighting;
        }
        if (settings_.lod_scale > 0.0f) {
            g_alpine_game_config.set_lod_dist_scale(settings_.lod_scale);
        }
        // The full-resolution HUD is shrunk onto a quad in front of the eyes;
        // the big HUD keeps its text readable there.
        if (settings_.big_hud >= 0 && g_alpine_game_config.big_hud != (settings_.big_hud == 1)) {
            g_alpine_game_config.big_hud = settings_.big_hud == 1;
            set_big_hud(g_alpine_game_config.big_hud);
        }

        xlog::info(
            "[AFVR][WinlatorXR] Settings ({}): eye_height={} fov_scale={:.2f} hud={:.2f}m@{:.2f}m grip_pitch={:.1f} flip_controllers={} sync_wait={}ms block_keyboard={}",
            write_time ? path : std::string{"defaults"}, settings_.eye_height, settings_.fov_scale,
            settings_.hud_width_m, settings_.hud_distance_m, settings_.grip_pitch_deg,
            settings_.flip_controllers, settings_.sync_wait_ms, settings_.block_keyboard ? 1 : 0);
        xlog::info("[AFVR][WinlatorXR] Graphics: shadow_quality={} mesh_lighting={} lod_scale={:.2f} big_hud={}",
            g_alpine_game_config.shadow_quality, g_alpine_game_config.mesh_lighting_mode,
            g_alpine_game_config.lod_dist_scale, g_alpine_game_config.big_hud ? 1 : 0);
    }

    void WinlatorXrContext::receive_loop()
    {
        std::array<char, 2048> buffer{};
        uint64_t sequence = 0;
        while (receiver_running_) {
            const int received = recvfrom(as_socket(receive_socket_), buffer.data(),
                static_cast<int>(buffer.size() - 1), 0, nullptr, nullptr);
            if (!receiver_running_) {
                break;
            }
            if (received <= 0) {
                continue; // timeout or transient error
            }
            buffer[received] = '\0';
            ++datagrams_received_;
            std::string raw{buffer.data(), static_cast<size_t>(received)};
            WinlatorXrPacket packet;
            if (!parse_winlatorxr_packet(buffer.data(), packet)) {
                ++datagrams_rejected_;
                std::lock_guard lock{packet_mutex_};
                if (rejected_example_.empty()) {
                    rejected_example_ = raw.substr(0, 400);
                }
                continue;
            }
            packet.sequence = ++sequence;
            {
                std::lock_guard lock{packet_mutex_};
                latest_packet_ = packet;
            }
            packet_cv_.notify_all();
        }
    }

    void WinlatorXrContext::send_state(int mode_vr, int mode_3d, float fov_x, float fov_y)
    {
        if (as_socket(send_socket_) == INVALID_SOCKET) {
            return;
        }
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(send_port);
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const std::string message = std::format("0 0 {} {} {:.3f} {:.3f}",
            mode_vr, mode_3d, fov_x, fov_y);
        sendto(as_socket(send_socket_), message.c_str(), static_cast<int>(message.size()), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        if (mode_vr != last_mode_vr_ || mode_3d != last_mode_3d_) {
            last_mode_vr_ = mode_vr;
            last_mode_3d_ = mode_3d;
            xlog::info("[AFVR][WinlatorXR] Display mode: {} ({})", mode_vr == 1 ? "stereo VR" : "flat screen",
                message);
        }
    }

    void WinlatorXrContext::ensure_window()
    {
        HWND window = rf::main_wnd;
        if (!window) {
            return;
        }
        const int width = GetSystemMetrics(SM_CXSCREEN);
        const int height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0 || height <= 0) {
            return;
        }
        // WinlatorXR reads the frame-sync stamp at screen (0,0) and splits the
        // whole X screen into the two eyes: keep the window borderless there.
        const LONG_PTR style = GetWindowLongPtrA(window, GWL_STYLE);
        const LONG_PTR wanted_style = (style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME |
            WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
        bool changed = false;
        if (wanted_style != style) {
            SetWindowLongPtrA(window, GWL_STYLE, wanted_style);
            changed = true;
        }
        const LONG_PTR ex_style = GetWindowLongPtrA(window, GWL_EXSTYLE);
        const LONG_PTR wanted_ex_style = ex_style & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE |
            WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
        if (wanted_ex_style != ex_style) {
            SetWindowLongPtrA(window, GWL_EXSTYLE, wanted_ex_style);
            changed = true;
        }
        RECT rect{};
        const bool geometry_wrong = !GetWindowRect(window, &rect) || rect.left != 0 ||
            rect.top != 0 || rect.right - rect.left != width || rect.bottom - rect.top != height;
        if (changed || geometry_wrong) {
            SetWindowPos(window, HWND_TOP, 0, 0, width, height,
                SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
            xlog::info("[AFVR][WinlatorXR] Pinned game window borderless at (0,0) {}x{} (was {},{} {}x{})",
                width, height, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
            if (rf::gr::screen.max_w != width || rf::gr::screen.max_h != height) {
                xlog::warn(
                    "[AFVR][WinlatorXR] Game resolution {}x{} differs from the container screen {}x{}; set the same resolution in the launcher for a sharp image",
                    rf::gr::screen.max_w, rf::gr::screen.max_h, width, height);
            }
        }
    }

    bool WinlatorXrContext::poll_events()
    {
        reload_settings(false);
        if (!initialized_) {
            return false;
        }
        ensure_window();
        if (!session_running_) {
            // vr.cpp only waits for frames once the session runs; start it from
            // the first usable tracking packet.
            (void)consume_packet(false);
        }
        return true;
    }

    bool WinlatorXrContext::wait_frame()
    {
        if (!initialized_) {
            return false;
        }
        return consume_packet(true);
    }

    bool WinlatorXrContext::consume_packet(bool wait)
    {
        // Pace the game to the headset like xrWaitFrame: wait briefly for a
        // pose newer than the one the previous frame was rendered with.
        std::unique_lock lock{packet_mutex_};
        if (wait && session_running_ && settings_.sync_wait_ms > 0) {
            packet_cv_.wait_for(lock, std::chrono::milliseconds(settings_.sync_wait_ms), [this] {
                return latest_packet_.sequence != consumed_sequence_;
            });
        }
        frame_packet_ = latest_packet_;
        lock.unlock();

        if (!poll_logged_) {
            poll_logged_ = true;
            xlog::info("[AFVR][WinlatorXR] Frame loop active (packet valid={}, sequence={})",
                frame_packet_.valid, frame_packet_.sequence);
        }

        if (!frame_packet_.valid) {
            return false;
        }
        consumed_sequence_ = frame_packet_.sequence;

        if (!first_packet_logged_) {
            first_packet_logged_ = true;
            xlog::info(
                "[AFVR][WinlatorXR] First tracking packet: head=({:.3f},{:.3f},{:.3f}) ipd={:.4f} fov={:.1f}x{:.1f} altitude={:.2f}",
                frame_packet_.head.position.x, frame_packet_.head.position.y,
                frame_packet_.head.position.z, frame_packet_.ipd, frame_packet_.fov_h_deg,
                frame_packet_.fov_v_deg, frame_packet_.hmd_altitude);
        }

        // WinlatorXR echoes back the FOV we send, so latch the native FOV from
        // the first plausible packet only.
        if (native_fov_h_deg_ <= 1.0f &&
            frame_packet_.fov_h_deg >= 40.0f && frame_packet_.fov_h_deg <= 160.0f &&
            frame_packet_.fov_v_deg >= 40.0f && frame_packet_.fov_v_deg <= 160.0f) {
            native_fov_h_deg_ = frame_packet_.fov_h_deg;
            native_fov_v_deg_ = frame_packet_.fov_v_deg;
            xlog::info("[AFVR][WinlatorXR] Native eye FOV latched: {:.1f} x {:.1f} degrees (aspect {:.3f})",
                native_fov_h_deg_, native_fov_v_deg_,
                std::tan(native_fov_h_deg_ * 0.00872665f) / std::tan(native_fov_v_deg_ * 0.00872665f));
        }
        if (native_fov_h_deg_ > 1.0f) {
            constexpr float half_degree_to_radian = 3.14159265f / 360.0f;
            tan_half_h_ = std::tan(native_fov_h_deg_ * settings_.fov_scale * half_degree_to_radian);
            tan_half_v_ = std::tan(native_fov_v_deg_ * settings_.fov_scale * half_degree_to_radian);
        }

        if (!session_running_ && native_fov_h_deg_ > 1.0f) {
            session_running_ = true;
            session_state_ = XR_SESSION_STATE_FOCUSED;
            xlog::info("[AFVR][WinlatorXR] Tracking active; VR session running");
        }
        return session_running_;
    }

    XrPosef WinlatorXrContext::aim_pose(const XrPosef& raw) const
    {
        XrPosef pose = raw;
        if (!normalize(pose.orientation)) {
            pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        // XrAPI's controller rotation is rolled 180 degrees about the aim
        // axis compared to an OpenXR aim pose on every headset.
        pose.orientation = multiply(pose.orientation, XrQuaternionf{0.0f, 0.0f, 1.0f, 0.0f});
        const bool flip = settings_.flip_controllers < 0 ? flip_controllers_auto_
            : settings_.flip_controllers == 1;
        if (flip) {
            // Quest 2/Pro/Pico additionally report the controller upside down:
            // 180-degree turn about the controller's X axis.
            pose.orientation = multiply(pose.orientation, XrQuaternionf{1.0f, 0.0f, 0.0f, 0.0f});
        }
        return pose;
    }

    XrPosef WinlatorXrContext::grip_pose(const XrPosef& aim) const
    {
        XrPosef pose = aim;
        if (settings_.grip_pitch_deg != 0.0f) {
            const float half = settings_.grip_pitch_deg * 3.14159265f / 360.0f;
            pose.orientation = multiply(pose.orientation,
                XrQuaternionf{std::sin(half), 0.0f, 0.0f, std::cos(half)});
        }
        return pose;
    }

    bool WinlatorXrContext::sync_actions()
    {
        if (!session_running_ || !frame_packet_.valid) {
            input_state_ = {};
            return false;
        }
        const auto& p = frame_packet_;
        auto& in = input_state_;
        in.left_thumbstick = p.thumbsticks[0];
        in.right_thumbstick = p.thumbsticks[1];
        in.left_thumbstick_click = p.buttons[wxr_left_thumbstick];
        in.right_thumbstick_click = p.buttons[wxr_right_thumbstick];
        // XrAPI reports triggers and grips as digital buttons only.
        in.left_trigger = p.buttons[wxr_left_trigger] ? 1.0f : 0.0f;
        in.right_trigger = p.buttons[wxr_right_trigger] ? 1.0f : 0.0f;
        in.grip[0] = p.buttons[wxr_left_grip] ? 1.0f : 0.0f;
        in.grip[1] = p.buttons[wxr_right_grip] ? 1.0f : 0.0f;
        in.reload = p.buttons[wxr_left_x];
        in.flashlight = p.buttons[wxr_left_y];
        in.crouch = p.buttons[wxr_right_a];
        in.jump = p.buttons[wxr_right_b];
        in.menu = p.buttons[wxr_left_menu];
        in.index_profile_active = false;
        for (size_t hand = 0; hand < 2; ++hand) {
            const XrPosef aim = aim_pose(p.hands[hand]);
            in.aim_poses[hand] = aim;
            in.grip_poses[hand] = grip_pose(aim);
            const bool valid = std::isfinite(aim.position.x) && std::isfinite(aim.position.y) &&
                std::isfinite(aim.position.z);
            in.aim_pose_valid[hand] = valid;
            in.grip_pose_valid[hand] = valid;
        }
        return true;
    }

    XrPosef WinlatorXrContext::current_center_pose() const
    {
        return frame_packet_.head;
    }

    bool WinlatorXrContext::ensure_eye_targets()
    {
        const int height = settings_.eye_height > 0 ? settings_.eye_height
            : std::max(rf::gr::screen.max_h, 256);
        const int width = std::max(static_cast<int>(std::lround(
            static_cast<float>(height) * tan_half_h_ / std::max(tan_half_v_, 0.01f))), 64);
        if (eyes_[0].width == width && eyes_[0].height == height && eyes_[0].rtv) {
            return true;
        }
        for (auto& eye : eyes_) {
            eye = {};
            D3D11_TEXTURE2D_DESC color_desc{};
            color_desc.Width = width;
            color_desc.Height = height;
            color_desc.MipLevels = 1;
            color_desc.ArraySize = 1;
            color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            color_desc.SampleDesc.Count = 1;
            color_desc.Usage = D3D11_USAGE_DEFAULT;
            color_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            D3D11_TEXTURE2D_DESC depth_desc = color_desc;
            depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (FAILED(device_->CreateTexture2D(&color_desc, nullptr, &eye.color)) ||
                FAILED(device_->CreateRenderTargetView(eye.color, nullptr, &eye.rtv)) ||
                FAILED(device_->CreateShaderResourceView(eye.color, nullptr, &eye.srv)) ||
                FAILED(device_->CreateTexture2D(&depth_desc, nullptr, &eye.depth)) ||
                FAILED(device_->CreateDepthStencilView(eye.depth, nullptr, &eye.dsv))) {
                xlog::error("[AFVR][WinlatorXR] Could not create {}x{} eye targets", width, height);
                eye = {};
                return false;
            }
            eye.width = width;
            eye.height = height;
        }
        xlog::info("[AFVR][WinlatorXR] Eye render targets: {}x{} (back buffer {}x{}, side by side)",
            width, height, rf::gr::screen.max_w, rf::gr::screen.max_h);
        return true;
    }

    bool WinlatorXrContext::ensure_hud_target()
    {
        const int width = std::max(rf::gr::screen.max_w, 1);
        const int height = std::max(rf::gr::screen.max_h, 1);
        if (hud_.width == width && hud_.height == height && hud_.rtv) {
            return true;
        }
        hud_ = {};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        // Full mip chain: the HUD is minified several times on the headset
        // and would alias into unreadable text without mipmaps.
        desc.MipLevels = 0;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &hud_.color)) ||
            FAILED(device_->CreateRenderTargetView(hud_.color, nullptr, &hud_.rtv)) ||
            FAILED(device_->CreateShaderResourceView(hud_.color, nullptr, &hud_.srv))) {
            xlog::error("[AFVR][WinlatorXR] Could not create the {}x{} HUD target", width, height);
            hud_ = {};
            return false;
        }
        hud_.width = width;
        hud_.height = height;
        return true;
    }

    bool WinlatorXrContext::render_frame(const OpenXrEyeRenderCallback& render_eye,
        const OpenXrHudRenderCallback& render_hud)
    {
        compose_pending_ = false;
        if (!session_running_ || !frame_packet_.valid || !ensure_eye_targets()) {
            return false;
        }

        const XrPosef center = current_center_pose();
        const XrVector3f right = rotate(center.orientation, {1.0f, 0.0f, 0.0f});
        float ipd = frame_packet_.ipd > 1.0f ? frame_packet_.ipd * 0.001f : frame_packet_.ipd;
        if (ipd < 0.045f || ipd > 0.085f) {
            ipd = 0.064f;
        }
        const float half_h = std::atan(tan_half_h_);
        const float half_v = std::atan(tan_half_v_);

        for (uint32_t eye_index = 0; eye_index < 2; ++eye_index) {
            const float side = eye_index == 0 ? -0.5f : 0.5f;
            XrView view{XR_TYPE_VIEW};
            view.pose.orientation = center.orientation;
            view.pose.position = {
                center.position.x + right.x * ipd * side,
                center.position.y + right.y * ipd * side,
                center.position.z + right.z * ipd * side,
            };
            view.fov = {-half_h, half_h, half_v, -half_v};
            auto& eye = eyes_[eye_index];
            render_eye(OpenXrEyeRenderInfo{
                eye_index, view, center, eye.rtv, eye.srv, eye.dsv, eye.width, eye.height,
            });
        }

        bool hud_rendered = false;
        if (hud_active_ && render_hud && ensure_hud_target()) {
            render_hud(OpenXrHudRenderInfo{hud_.rtv, hud_.width, hud_.height});
            hud_rendered = true;
            if (!first_hud_logged_) {
                first_hud_logged_ = true;
                xlog::info("[AFVR][WinlatorXR] Gameplay HUD composed into both eyes ({}x{})",
                    hud_.width, hud_.height);
            }
        }

        compose_info_ = {};
        compose_info_.eye_views[0] = eyes_[0].srv;
        compose_info_.eye_views[1] = eyes_[1].srv;
        compose_info_.sync_value = frame_packet_.sync;
        if (hud_rendered) {
            compose_info_.hud_view = hud_.srv;
            compose_info_.hud_generate_mips = true;
            // Head-locked HUD at hud_distance_m, converged for each eye. Only
            // the outer hud_crop of each HUD side is shown, as two strips
            // separated by hud_gap_m, so health and ammo sit nearer the center.
            const float distance = std::max(settings_.hud_distance_m, 0.1f);
            const float crop = settings_.hud_crop;
            const float strip_width = settings_.hud_width_m * crop / distance;
            const float half_gap = 0.5f * settings_.hud_gap_m / distance;
            const float half_height = 0.5f * settings_.hud_width_m * static_cast<float>(hud_.height) /
                static_cast<float>(hud_.width) / distance;
            const float center_y = settings_.hud_height_m / distance;
            compose_info_.hud_crop = crop;
            for (int eye_index = 0; eye_index < 2; ++eye_index) {
                const float shift = (eye_index == 0 ? 0.5f : -0.5f) * ipd / distance;
                const float lefts[2] = {shift - half_gap - strip_width, shift + half_gap};
                for (int strip = 0; strip < 2; ++strip) {
                    auto& rect = compose_info_.hud_rect[eye_index][strip];
                    rect[0] = lefts[strip] / tan_half_h_;
                    rect[1] = (lefts[strip] + strip_width) / tan_half_h_;
                    rect[2] = (center_y - half_height) / tan_half_v_;
                    rect[3] = (center_y + half_height) / tan_half_v_;
                }
            }
        }
        compose_pending_ = true;
        if (!first_stereo_logged_) {
            first_stereo_logged_ = true;
            xlog::info("[AFVR][WinlatorXR] First stereo world frame rendered (ipd {:.4f} m)", ipd);
        }
        return true;
    }

    bool WinlatorXrContext::ensure_screen_target()
    {
        const int width = std::max(rf::gr::screen.max_w, 1);
        const int height = std::max(rf::gr::screen.max_h, 1);
        if (screen_.width == width && screen_.height == height && screen_.srv) {
            return true;
        }
        screen_ = {};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        // Same format as the swap chain (the presented menu is copied into
        // mip 0) and a mip chain so the minified menu text stays readable.
        desc.MipLevels = 0;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &screen_.color)) ||
            FAILED(device_->CreateShaderResourceView(screen_.color, nullptr, &screen_.srv))) {
            xlog::error("[AFVR][WinlatorXR] Could not create the {}x{} menu screen", width, height);
            screen_ = {};
            return false;
        }
        screen_.width = width;
        screen_.height = height;
        return true;
    }

    // Menus and the zoomed rifle scope: WinlatorXR's own flat screen mode
    // shows the side-by-side window magnified and doubled, so the flat game
    // image is drawn into both eyes as a virtual screen fixed in the room
    // (like the OpenXR quad layer on PC) and the frame stays stereo.
    bool WinlatorXrContext::render_screen_frame(const OpenXrMenuRenderCallback& render)
    {
        compose_pending_ = false;
        if (!session_running_ || !frame_packet_.valid || !ensure_screen_target()) {
            return false;
        }
        const XrPosef center = current_center_pose();
        const float distance = std::max(settings_.screen_distance_m, 0.3f);
        if (!screen_anchor_valid_) {
            XrVector3f forward = rotate(center.orientation, {0.0f, 0.0f, -1.0f});
            forward.y = 0.0f;
            const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
            if (length < 0.001f) {
                forward = {0.0f, 0.0f, -1.0f};
            }
            else {
                forward.x /= length;
                forward.z /= length;
            }
            // Yaw-only orientation whose -Z axis is the horizontal forward.
            const float yaw = std::atan2(-forward.x, -forward.z);
            screen_pose_.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            screen_pose_.position = {
                center.position.x + forward.x * distance,
                center.position.y,
                center.position.z + forward.z * distance,
            };
            screen_anchor_valid_ = true;
            xlog::info("[AFVR][WinlatorXR] Menu/scope screen fixed {:.1f} m ahead, {:.2f} m wide",
                distance, settings_.screen_width_m);
        }

        render(OpenXrMenuRenderInfo{screen_.color, center, screen_.width, screen_.height});

        float ipd = frame_packet_.ipd > 1.0f ? frame_packet_.ipd * 0.001f : frame_packet_.ipd;
        if (ipd < 0.045f || ipd > 0.085f) {
            ipd = 0.064f;
        }
        const XrVector3f head_right = rotate(center.orientation, {1.0f, 0.0f, 0.0f});
        const XrQuaternionf inverse_head{-center.orientation.x, -center.orientation.y,
            -center.orientation.z, center.orientation.w};
        const float half_w = 0.5f * settings_.screen_width_m;
        const float half_h = half_w * static_cast<float>(screen_.height) / static_cast<float>(screen_.width);
        const XrVector3f screen_right = rotate(screen_pose_.orientation, {half_w, 0.0f, 0.0f});
        const XrVector3f screen_up = rotate(screen_pose_.orientation, {0.0f, half_h, 0.0f});
        const float corner_signs[4][2] = {{-1.0f, 1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}};

        compose_info_ = {};
        compose_info_.screen_view = screen_.srv;
        compose_info_.sync_value = frame_packet_.sync;
        for (int eye_index = 0; eye_index < 2; ++eye_index) {
            const float side = eye_index == 0 ? -0.5f : 0.5f;
            const XrVector3f eye_position{
                center.position.x + head_right.x * ipd * side,
                center.position.y + head_right.y * ipd * side,
                center.position.z + head_right.z * ipd * side,
            };
            for (int corner = 0; corner < 4; ++corner) {
                const float sx = corner_signs[corner][0];
                const float sy = corner_signs[corner][1];
                const XrVector3f world{
                    screen_pose_.position.x + screen_right.x * sx + screen_up.x * sy - eye_position.x,
                    screen_pose_.position.y + screen_right.y * sx + screen_up.y * sy - eye_position.y,
                    screen_pose_.position.z + screen_right.z * sx + screen_up.z * sy - eye_position.z,
                };
                const XrVector3f view = rotate(inverse_head, world);
                auto& clip = compose_info_.screen_clip[eye_index][corner];
                clip[0] = view.x / tan_half_h_;
                clip[1] = view.y / tan_half_v_;
                clip[2] = 0.0f;
                clip[3] = -view.z;
            }
        }
        compose_pending_ = true;
        return true;
    }

    bool WinlatorXrContext::render_menu_frame(const OpenXrMenuRenderCallback& render_menu)
    {
        if (!menu_active_) {
            return false;
        }
        if (settings_.menu_2d) {
            // WinlatorXR's own 2D screen shows the full-resolution window and
            // drives the cursor with its controller-to-mouse emulation.
            compose_pending_ = false;
            if (!session_running_ || !frame_packet_.valid) {
                return false;
            }
            render_menu(OpenXrMenuRenderInfo{nullptr, current_center_pose(), 0, 0});
            return true;
        }
        return render_screen_frame(render_menu);
    }

    bool WinlatorXrContext::render_scope_frame(const OpenXrMenuRenderCallback& render_scope)
    {
        if (!scope_active_) {
            return false;
        }
        return render_screen_frame(render_scope);
    }

    void WinlatorXrContext::set_menu_active(bool active)
    {
        if (menu_active_ != active) {
            screen_anchor_valid_ = false;
        }
        menu_active_ = active;
    }

    void WinlatorXrContext::set_scope_active(bool active)
    {
        if (scope_active_ != active) {
            screen_anchor_valid_ = false;
            xlog::info("[AFVR][WinlatorXR] Native weapon scope {}", active ? "shown on the virtual screen" : "closed");
        }
        scope_active_ = active;
    }

    void WinlatorXrContext::set_hud_active(bool active)
    {
        hud_active_ = active;
    }

    void WinlatorXrContext::reset_fixed_quad_poses()
    {
        screen_anchor_valid_ = false;
    }

    bool WinlatorXrContext::screen_ray_uv(const XrPosef& aim, float& u, float& v) const
    {
        const XrVector3f origin{
            aim.position.x - screen_pose_.position.x,
            aim.position.y - screen_pose_.position.y,
            aim.position.z - screen_pose_.position.z,
        };
        const XrVector3f direction = rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
        const XrQuaternionf inverse_screen{-screen_pose_.orientation.x, -screen_pose_.orientation.y,
            -screen_pose_.orientation.z, screen_pose_.orientation.w};
        const XrVector3f local_origin = rotate(inverse_screen, origin);
        const XrVector3f local_direction = rotate(inverse_screen, direction);
        if (std::abs(local_direction.z) < 0.0001f) {
            return false;
        }
        const float t = -local_origin.z / local_direction.z;
        if (t <= 0.0f) {
            return false;
        }
        const float width = settings_.screen_width_m;
        const float height = width * static_cast<float>(screen_.height) / static_cast<float>(screen_.width);
        u = (local_origin.x + local_direction.x * t) / width + 0.5f;
        v = 0.5f - (local_origin.y + local_direction.y * t) / height;
        return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
    }

    bool WinlatorXrContext::get_menu_pointer(float& u, float& v) const
    {
        constexpr size_t right_hand = 1;
        if (!menu_active_ || settings_.menu_2d || !screen_anchor_valid_ || screen_.width <= 0 ||
            !input_state_.aim_pose_valid[right_hand]) {
            return false;
        }
        return screen_ray_uv(input_state_.aim_poses[right_hand], u, v);
    }

    bool WinlatorXrContext::take_compose_info(WinlatorXrComposeInfo& info)
    {
        if (!compose_pending_) {
            return false;
        }
        info = compose_info_;
        return true;
    }

    void WinlatorXrContext::frame_presented(bool stereo)
    {
        compose_pending_ = false;
        float fov_x = 0.0f;
        float fov_y = 0.0f;
        // FOV 0 0 in screen mode selects WinlatorXR's default screen size; the
        // eye FOV there blows the 2D screen up over the whole view.
        if (stereo && native_fov_h_deg_ > 1.0f) {
            fov_x = native_fov_h_deg_ * settings_.fov_scale;
            fov_y = native_fov_v_deg_ * settings_.fov_scale;
        }
        send_state(stereo ? 1 : 2, stereo ? 1 : 0, fov_x, fov_y);

        ++presented_frames_;
        if (stereo) {
            ++stereo_frames_;
        }
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - fps_window_start_).count();
        if (seconds >= 10.0) {
            xlog::info("[AFVR][WinlatorXR] {:.1f} fps ({} stereo / {} presented, eye {}x{}; {} packets, {} rejected; window active {})",
                presented_frames_ / seconds, stereo_frames_, presented_frames_,
                eyes_[0].width, eyes_[0].height, datagrams_received_.load(), datagrams_rejected_.load(),
                rf::is_main_wnd_active);
            {
                std::lock_guard lock{packet_mutex_};
                if (!rejected_example_.empty() && !rejected_example_logged_) {
                    rejected_example_logged_ = true;
                    xlog::warn("[AFVR][WinlatorXR] Unparsed XrAPI packet: {}", rejected_example_);
                }
            }
            presented_frames_ = 0;
            stereo_frames_ = 0;
            fps_window_start_ = now;

            const uint32_t datagrams = datagrams_received_.load();
            const bool stale = session_running_ && datagrams == last_window_datagrams_;
            last_window_datagrams_ = datagrams;
            if (stale && !packet_timeout_logged_) {
                packet_timeout_logged_ = true;
                xlog::warn("[AFVR][WinlatorXR] No new tracking packets; is the XR API enabled for this container?");
            }
        }
    }

    void WinlatorXrContext::shutdown()
    {
        receiver_running_ = false;
        if (as_socket(receive_socket_) != INVALID_SOCKET) {
            closesocket(as_socket(receive_socket_));
        }
        if (receiver_.joinable()) {
            receiver_.join();
        }
        receive_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
        if (as_socket(send_socket_) != INVALID_SOCKET) {
            if (initialized_) {
                send_state(0, 0, 0.0f, 0.0f);
            }
            closesocket(as_socket(send_socket_));
        }
        send_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
        if (wsa_started_) {
            WSACleanup();
            wsa_started_ = false;
        }
        for (auto& eye : eyes_) {
            eye = {};
        }
        hud_ = {};
        compose_pending_ = false;
        session_running_ = false;
        session_state_ = XR_SESSION_STATE_UNKNOWN;
        input_state_ = {};
        if (initialized_) {
            xlog::info("[AFVR][WinlatorXR] Backend shut down");
        }
        initialized_ = false;
    }
}

// SPDX-License-Identifier: MIT
// Music Player Plugin - Main Entry Point
//
// Background music player for MP3 files from /sd/music
// Controls:
//   META+Space: Pause/resume
//   META+Left:  Restart or previous track (if <10s)
//   META+Right: Next track
//   META+Up:    Show song info
//   Volume keys: Adjust volume

#include "tanmatsu_plugin.h"
#include "../include/music_player.h"
#include "playlist.h"
#include "audio.h"
#include "input_handler.h"
#include "widget.h"

// Global state
static music_player_state_t g_state = {0};
static plugin_context_t* g_ctx = NULL;

music_player_state_t* music_player_get_state(void) {
    return &g_state;
}

// Plugin metadata
static const plugin_info_t plugin_info = {
    .name = "Music Player",
    .slug = "at.cavac.musicplayer",
    .version = "1.0.0",
    .author = "Rene Schickbauer",
    .description = "Background MP3 music player",
    .api_version = TANMATSU_PLUGIN_API_VERSION,
    .type = PLUGIN_TYPE_SERVICE,
    .flags = 0,
};

static const plugin_info_t* get_info(void) {
    return &plugin_info;
}

static int plugin_init(plugin_context_t* ctx) {
    g_ctx = ctx;

    asp_log_info("musicplayer", "Initializing music player plugin...");

    // Initialize state
    g_state.state = PLAYBACK_STOPPED;
    g_state.volume = 100;  // Default 100% volume
    g_state.song_start_time = 0;
    g_state.current_position_ms = 0;

    // Load saved volume from settings
    int32_t saved_volume;
    if (asp_plugin_settings_get_int(ctx, "volume", &saved_volume)) {
        if (saved_volume >= 0 && saved_volume <= 100) {
            g_state.volume = (uint8_t)saved_volume;
            asp_log_info("musicplayer", "Loaded saved volume: %d%%", g_state.volume);
        }
    }

    // Initialize playlist (checks /sd/music)
    if (playlist_init() != 0) {
        asp_log_warn("musicplayer", "No music found, plugin will not start");
        return -1;  // Exit if no music
    }

    // Initialize audio subsystem
    if (audio_init() != 0) {
        asp_log_error("musicplayer", "Failed to initialize audio");
        playlist_cleanup();
        return -1;
    }

    // Set initial volume
    audio_set_volume(g_state.volume);

    // Register input hook
    if (input_handler_init(ctx) != 0) {
        asp_log_error("musicplayer", "Failed to register input hook");
        audio_cleanup();
        playlist_cleanup();
        return -1;
    }

    // Register status widget (optional - continue even if it fails)
    if (widget_init(ctx) != 0) {
        asp_log_warn("musicplayer", "Status widget not available");
    }

    asp_log_info("musicplayer", "Music player initialized with %d songs",
                 g_state.playlist.count);
    return 0;
}

static void plugin_cleanup(plugin_context_t* ctx) {
    asp_log_info("musicplayer", "Cleaning up music player...");

    // Save volume setting
    asp_plugin_settings_set_int(ctx, "volume", g_state.volume);

    // Stop playback
    audio_stop();
    g_state.state = PLAYBACK_STOPPED;

    // Cleanup in reverse order
    widget_cleanup();
    input_handler_cleanup();
    audio_cleanup();
    playlist_cleanup();

    g_ctx = NULL;
    asp_log_info("musicplayer", "Music player cleaned up");
}

static void plugin_service_run(plugin_context_t* ctx) {
    asp_log_info("musicplayer", "Music player service starting...");

    // Start playing first song
    if (g_state.playlist.count > 0) {
        const char* path = playlist_get_current_path();
        if (path) {
            audio_play_file(path);
            g_state.state = PLAYBACK_PLAYING;
            g_state.song_start_time = asp_plugin_get_tick_ms();
        }
    }

    // Main service loop
    while (!asp_plugin_should_stop(ctx)) {
        // Process audio (decode and play frames)
        if (g_state.state == PLAYBACK_PLAYING) {
            // Update position
            g_state.current_position_ms = audio_get_position_ms();

            // Process some audio frames
            if (!audio_process()) {
                // Song finished - advance to next
                if (audio_is_finished()) {
                    playlist_next();
                    const char* path = playlist_get_current_path();
                    if (path) {
                        audio_play_file(path);
                        g_state.song_start_time = asp_plugin_get_tick_ms();
                        asp_log_info("musicplayer", "Auto-advancing to next track");
                    }
                }
            }
        }

        // Small delay to avoid busy loop
        // When playing, we process frequently for smooth audio
        // When paused/stopped, we can sleep longer
        if (g_state.state == PLAYBACK_PLAYING) {
            asp_plugin_delay_ms(10);
        } else {
            asp_plugin_delay_ms(50);
        }
    }

    asp_log_info("musicplayer", "Music player service stopped");
}

// Plugin entry point structure
static const plugin_entry_t entry = {
    .get_info = get_info,
    .init = plugin_init,
    .cleanup = plugin_cleanup,
    .menu_render = NULL,
    .menu_select = NULL,
    .service_run = plugin_service_run,
    .hook_event = NULL,
};

// Register this plugin with the host
TANMATSU_PLUGIN_REGISTER(entry);

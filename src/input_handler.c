// SPDX-License-Identifier: MIT
// Music Player Plugin - Input Handler
// Uses modifier flags from navigation events instead of manual META tracking

#include "input_handler.h"
#include "audio.h"
#include "playlist.h"
#include "../include/music_player.h"
#include "tanmatsu_plugin.h"
#include <stdio.h>

// Modifier flags (from bsp/input.h)
#define BSP_INPUT_MODIFIER_SUPER_L   (1 << 7)
#define BSP_INPUT_MODIFIER_SUPER_R   (1 << 8)
#define BSP_INPUT_MODIFIER_SUPER     (BSP_INPUT_MODIFIER_SUPER_L | BSP_INPUT_MODIFIER_SUPER_R)

// Navigation keys (from bsp/input.h enum - counted from 0)
// BSP_INPUT_NAVIGATION_KEY_NONE = 0
// BSP_INPUT_NAVIGATION_KEY_ESC = 1
// BSP_INPUT_NAVIGATION_KEY_LEFT = 2
// BSP_INPUT_NAVIGATION_KEY_RIGHT = 3
// BSP_INPUT_NAVIGATION_KEY_UP = 4
// BSP_INPUT_NAVIGATION_KEY_DOWN = 5
// ... etc
#define NAV_KEY_LEFT        2
#define NAV_KEY_RIGHT       3
#define NAV_KEY_UP          4
#define NAV_KEY_DOWN        5
#define NAV_KEY_SELECT      12
#define NAV_KEY_SPACE_L     16
#define NAV_KEY_SPACE_M     17
#define NAV_KEY_SPACE_R     18
#define NAV_KEY_VOLUME_UP   37
#define NAV_KEY_VOLUME_DOWN 38

static int g_hook_id = -1;

// Show song info dialog
static void show_song_info(void) {
    music_player_state_t* state = music_player_get_state();
    const char* filename = playlist_get_current_filename();

    if (!filename) return;

    // Build info lines
    static char line1[64];
    static char line2[128];
    static char line3[64];
    static char line4[64];

    snprintf(line1, sizeof(line1), "Now Playing:");
    snprintf(line2, sizeof(line2), "%s", filename);
    snprintf(line3, sizeof(line3), "Track %d of %d",
             state->playlist.current_index + 1, state->playlist.count);
    snprintf(line4, sizeof(line4), "Volume: %d%%", state->volume);

    const char* lines[] = { line1, line2, line3, line4 };

    asp_plugin_show_text_dialog("Music Player", lines, 4, 5000);  // 5 second timeout
}

// Input hook callback
static bool input_hook_callback(plugin_input_event_t* event, void* user_data) {
    music_player_state_t* state = music_player_get_state();
    (void)user_data;

    // Handle navigation events - these have proper modifier flags
    if (event->type == PLUGIN_INPUT_EVENT_TYPE_NAVIGATION) {
        // Only process key press events
        if (!event->state) {
            return false;
        }

        // Check if SUPER (meta/logo) modifier is held
        bool super_held = (event->modifiers & BSP_INPUT_MODIFIER_SUPER) != 0;

        // SUPER + Up: Show song info
        if (super_held && event->key == NAV_KEY_UP) {
            asp_log_info("musicplayer", "SUPER+UP: Show info");
            show_song_info();
            return true;  // Consume event
        }

        // SUPER + Left: Previous/restart
        if (super_held && event->key == NAV_KEY_LEFT) {
            asp_log_info("musicplayer", "SUPER+LEFT: Previous");
            int old_index = state->playlist.current_index;
            playlist_prev_or_restart();

            const char* path = playlist_get_current_path();
            if (path) {
                audio_play_file(path);
                state->song_start_time = asp_plugin_get_tick_ms();
                state->state = PLAYBACK_PLAYING;

                if (state->playlist.current_index != old_index) {
                    asp_log_info("musicplayer", "Previous track");
                } else {
                    asp_log_info("musicplayer", "Restart track");
                }
            }
            return true;  // Consume event
        }

        // SUPER + Right: Next song
        if (super_held && event->key == NAV_KEY_RIGHT) {
            asp_log_info("musicplayer", "SUPER+RIGHT: Next");
            playlist_next();
            const char* path = playlist_get_current_path();
            if (path) {
                audio_play_file(path);
                state->song_start_time = asp_plugin_get_tick_ms();
                state->state = PLAYBACK_PLAYING;
                asp_log_info("musicplayer", "Next track");
            }
            return true;  // Consume event
        }

        // SUPER + Down: Toggle pause/play
        if (super_held && event->key == NAV_KEY_DOWN) {
            asp_log_info("musicplayer", "SUPER+DOWN: Pause/play");
            if (state->state == PLAYBACK_PLAYING) {
                audio_pause();
                state->state = PLAYBACK_PAUSED;
                asp_log_info("musicplayer", "Paused");
            } else if (state->state == PLAYBACK_PAUSED) {
                audio_resume();
                state->state = PLAYBACK_PLAYING;
                asp_log_info("musicplayer", "Resumed");
            }
            return true;  // Consume event
        }

        // SUPER + Select: Also toggle pause/play (alternative)
        if (super_held && event->key == NAV_KEY_SELECT) {
            asp_log_info("musicplayer", "SUPER+SELECT: Pause/play");
            if (state->state == PLAYBACK_PLAYING) {
                audio_pause();
                state->state = PLAYBACK_PAUSED;
                asp_log_info("musicplayer", "Paused");
            } else if (state->state == PLAYBACK_PAUSED) {
                audio_resume();
                state->state = PLAYBACK_PLAYING;
                asp_log_info("musicplayer", "Resumed");
            }
            return true;  // Consume event
        }

        // SUPER + Space: Pause/Play (any of the three space keys)
        if (super_held && (event->key == NAV_KEY_SPACE_L ||
                           event->key == NAV_KEY_SPACE_M ||
                           event->key == NAV_KEY_SPACE_R)) {
            asp_log_info("musicplayer", "SUPER+SPACE: Pause/play");
            if (state->state == PLAYBACK_PLAYING) {
                audio_pause();
                state->state = PLAYBACK_PAUSED;
                asp_log_info("musicplayer", "Paused");
            } else if (state->state == PLAYBACK_PAUSED) {
                audio_resume();
                state->state = PLAYBACK_PLAYING;
                asp_log_info("musicplayer", "Resumed");
            }
            return true;  // Consume event
        }

        // SUPER + Volume up: Increase volume
        if (super_held && event->key == NAV_KEY_VOLUME_UP) {
            if (state->volume <= 95) {
                state->volume += 5;
            } else {
                state->volume = 100;
            }
            audio_set_volume(state->volume);
            asp_log_info("musicplayer", "Volume: %d%%", state->volume);
            return true;  // Consume event
        }

        // SUPER + Volume down: Decrease volume
        if (super_held && event->key == NAV_KEY_VOLUME_DOWN) {
            if (state->volume >= 5) {
                state->volume -= 5;
            } else {
                state->volume = 0;
            }
            audio_set_volume(state->volume);
            asp_log_info("musicplayer", "Volume: %d%%", state->volume);
            return true;  // Consume event
        }
    }

    return false;  // Don't consume unhandled events
}

int input_handler_init(plugin_context_t* ctx) {
    g_hook_id = asp_plugin_input_hook_register(ctx, input_hook_callback, NULL);
    if (g_hook_id < 0) {
        asp_log_error("musicplayer", "Failed to register input hook");
        return -1;
    }
    asp_log_info("musicplayer", "Input hook registered: %d", g_hook_id);
    return 0;
}

void input_handler_cleanup(void) {
    if (g_hook_id >= 0) {
        asp_plugin_input_hook_unregister(g_hook_id);
        g_hook_id = -1;
    }
}

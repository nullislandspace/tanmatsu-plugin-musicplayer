// SPDX-License-Identifier: MIT
// Music Player Plugin - Status Bar Widget

#include "widget.h"
#include "../include/music_player.h"
#include "tanmatsu_plugin.h"
#include "pax_gfx.h"

// External font used by launcher status bar (exported to plugins)
extern const pax_font_t chakrapetchmedium;

static int g_widget_id = -1;

// Format integer to string (snprintf not exported to plugins)
static int int_to_str(char* buf, int val) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    char temp[12];
    int i = 0;
    int negative = 0;

    if (val < 0) {
        negative = 1;
        val = -val;
    }

    while (val > 0) {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }

    int len = 0;
    if (negative) {
        buf[len++] = '-';
    }

    while (i > 0) {
        buf[len++] = temp[--i];
    }
    buf[len] = '\0';
    return len;
}

// Widget callback - draws status in header bar
// Returns width used
static int status_widget_callback(pax_buf_t* buffer, int x_right, int y, int height, void* user_data) {
    (void)user_data;
    music_player_state_t* state = music_player_get_state();

    // Don't show if not active
    if (state->state == PLAYBACK_STOPPED && state->playlist.count == 0) {
        return 0;
    }

    // Build status text
    char text[32];
    int text_len = 0;

    // Play/pause indicator
    if (state->state == PLAYBACK_PLAYING) {
        text[text_len++] = '>';  // Play symbol
        text[text_len++] = ' ';
    } else if (state->state == PLAYBACK_PAUSED) {
        text[text_len++] = '|';
        text[text_len++] = '|';
        text[text_len++] = ' ';
    } else {
        text[text_len++] = '-';
        text[text_len++] = ' ';
    }

    // Track number
    char num_buf[8];
    int num_len = int_to_str(num_buf, state->playlist.current_index + 1);
    for (int i = 0; i < num_len; i++) {
        text[text_len++] = num_buf[i];
    }

    text[text_len++] = '/';

    num_len = int_to_str(num_buf, state->playlist.count);
    for (int i = 0; i < num_len; i++) {
        text[text_len++] = num_buf[i];
    }

    // Add space and volume percentage
    text[text_len++] = ' ';
    num_len = int_to_str(num_buf, state->volume);
    for (int i = 0; i < num_len; i++) {
        text[text_len++] = num_buf[i];
    }
    text[text_len++] = '%';

    text[text_len] = '\0';

    // Calculate width and draw
    // Approximate character width for this font at size 16
    int char_width = 8;
    int text_width = text_len * char_width;
    int text_x = x_right - text_width - 4;
    int text_y = y + (height - 16) / 2;

    // Draw text
    pax_draw_text(buffer, 0xFF340132, &chakrapetchmedium, 16, text_x, text_y, text);

    return text_width + 8;  // Width used including margins
}

int widget_init(plugin_context_t* ctx) {
    g_widget_id = asp_plugin_status_widget_register(ctx, status_widget_callback, NULL);
    if (g_widget_id < 0) {
        asp_log_warn("musicplayer", "Failed to register status widget");
        return -1;
    }
    asp_log_info("musicplayer", "Status widget registered: %d", g_widget_id);
    return 0;
}

void widget_cleanup(void) {
    if (g_widget_id >= 0) {
        asp_plugin_status_widget_unregister(g_widget_id);
        g_widget_id = -1;
    }
}

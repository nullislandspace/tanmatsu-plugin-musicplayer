// SPDX-License-Identifier: MIT
// Music Player Plugin - Status Bar Widget

#pragma once

// Forward declaration
typedef struct plugin_context plugin_context_t;

// Initialize and register status widget
// Returns 0 on success, -1 on failure
int widget_init(plugin_context_t* ctx);

// Cleanup and unregister status widget
void widget_cleanup(void);

// SPDX-License-Identifier: MIT
// Music Player Plugin - Input Handler

#pragma once

// Forward declaration
typedef struct plugin_context plugin_context_t;

// Initialize input handler and register hook
// Returns 0 on success, -1 on failure
int input_handler_init(plugin_context_t* ctx);

// Cleanup input handler and unregister hook
void input_handler_cleanup(void);

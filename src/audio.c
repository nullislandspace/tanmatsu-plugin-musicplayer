// SPDX-License-Identifier: MIT
// Music Player Plugin - Audio Playback
// Uses minimp3 for MP3 decoding and ASP audio API for output
// Runs decoding in a separate pthread with larger stack to handle minimp3's stack usage

#include "audio.h"
#include "../include/music_player.h"
#include "tanmatsu_plugin.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

// Include minimp3 implementation
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

// ASP audio API - available to plugins
extern int asp_audio_set_rate(uint32_t rate_hz);
extern int asp_audio_get_volume(float* out_percentage);
extern int asp_audio_set_volume(float percentage);
extern int asp_audio_set_amplifier(bool enabled);
extern int asp_audio_stop(void);
extern int asp_audio_start(void);
extern int asp_audio_write(void* samples, size_t samples_size, int64_t timeout_ms);

// Buffer sizes
#define READ_BUFFER_SIZE    (16 * 1024)  // 16KB read buffer for file I/O (PSRAM)
#define MAX_FRAME_SIZE      (1152 * 2)   // Max samples per MP3 frame (stereo)
#define PCM_BUFFER_SIZE     (MAX_FRAME_SIZE * sizeof(int16_t))  // 4608 bytes

// Decoder thread stack size - minimp3 needs >16KB of stack (measured)
#define DECODER_STACK_SIZE  (32 * 1024)

// Audio state
static mp3dec_t* g_mp3_decoder = NULL;
static FILE* g_current_file = NULL;
static volatile bool g_playing = false;
static volatile bool g_paused = false;
static volatile bool g_song_finished = false;
static volatile uint64_t g_samples_written = 0;
static volatile uint32_t g_sample_rate = 44100;
static bool g_audio_initialized = false;

// PCM buffer in internal SRAM for DMA (16-byte aligned)
static int16_t pcm_buffer[MAX_FRAME_SIZE] __attribute__((aligned(16)));

// Heap-allocated buffers (PSRAM)
static uint8_t* read_buffer = NULL;
static size_t buffer_pos = 0;
static size_t buffer_len = 0;

// Decoder thread
static pthread_t decoder_thread;
static volatile bool g_thread_running = false;
static volatile bool g_thread_should_stop = false;
static volatile bool g_thread_in_decode = false;  // Track if thread is actively decoding

// Path for decoder thread to play
static char g_pending_path[256];
static volatile bool g_new_file_pending = false;

// Debug counter for fill_buffer
static int g_fill_count = 0;

// Track if we already warned about low buffer (to avoid spam)
static bool g_warned_buffer_low = false;

// Fill read buffer from file
static size_t fill_buffer(void) {
    if (!g_current_file) {
        return 0;
    }

    // Move remaining data to start of buffer
    if (buffer_pos > 0 && buffer_len > buffer_pos) {
        memmove(read_buffer, read_buffer + buffer_pos, buffer_len - buffer_pos);
        buffer_len -= buffer_pos;
        buffer_pos = 0;
    } else if (buffer_pos > 0) {
        buffer_len = 0;
        buffer_pos = 0;
    }

    // Read more data
    size_t space = READ_BUFFER_SIZE - buffer_len;
    if (space > 0) {
        size_t bytes_read = fread(read_buffer + buffer_len, 1, space, g_current_file);
        buffer_len += bytes_read;
        g_fill_count++;
    }

    return buffer_len - buffer_pos;
}

// Track if we've logged format for current file
static bool g_format_logged = false;

// Debug counters for audio clipping detection
static uint32_t g_clip_count = 0;
static uint32_t g_frame_count = 0;
static int16_t g_max_sample = 0;
static int16_t g_min_sample = 0;

// MP3 decode loop - decode frames and write PCM to audio output
static void decode_loop(void) {
    mp3dec_frame_info_t info;
    int samples;

    g_thread_in_decode = true;
    while (g_playing && !g_paused && !g_thread_should_stop) {
        // Ensure we have data in buffer
        size_t available = fill_buffer();

        // Debug: Check for buffer underrun (less than 1KB available)
        // Only warn once per low-buffer episode to avoid log spam
        if (available < 1024 && available >= 4) {
            if (!g_warned_buffer_low) {
                asp_log_warn("musicplayer", "Buffer low: %d bytes available", (int)available);
                g_warned_buffer_low = true;
            }
        } else if (available >= 4096) {
            // Reset warning flag when buffer is healthy again
            g_warned_buffer_low = false;
        }

        if (available < 4) {
            // End of file - stop playing to prevent decode_loop being called again
            g_song_finished = true;
            g_playing = false;
            asp_log_info("musicplayer", "Song finished (EOF, total clips=%u max=%d min=%d)",
                        g_clip_count, g_max_sample, g_min_sample);
            break;
        }

        // Debug: Check for potential buffer overrun
        if (buffer_pos > buffer_len) {
            asp_log_error("musicplayer", "Buffer overrun! pos=%d len=%d", (int)buffer_pos, (int)buffer_len);
        }

        // Decode one frame - track timing
        uint32_t decode_start = asp_plugin_get_tick_ms();
        samples = mp3dec_decode_frame(g_mp3_decoder,
                                       read_buffer + buffer_pos,
                                       buffer_len - buffer_pos,
                                       pcm_buffer, &info);
        uint32_t decode_time = asp_plugin_get_tick_ms() - decode_start;

        if (info.frame_bytes > 0) {
            buffer_pos += info.frame_bytes;
        }

        if (samples > 0) {
            g_frame_count++;

            // Warn if decode took too long (>20ms is concerning for real-time audio)
            if (decode_time > 20) {
                asp_log_warn("musicplayer", "Slow decode: frame %u took %u ms",
                            g_frame_count, (unsigned)decode_time);
            }

            // Debug: Scan for clipped samples and track min/max
            int total_samples = samples * info.channels;
            int clipped_this_frame = 0;
            for (int i = 0; i < total_samples; i++) {
                int16_t s = pcm_buffer[i];
                if (s > g_max_sample) g_max_sample = s;
                if (s < g_min_sample) g_min_sample = s;
                // Check if sample is at clipping boundary (after 0.7 scaling, this means original was way over)
                if (s >= 32767 || s <= -32768) {
                    clipped_this_frame++;
                }
                // Also check for values very close to limits (within 1% of max)
                if (s > 32440 || s < -32440) {
                    clipped_this_frame++;
                }
            }
            if (clipped_this_frame > 0) {
                g_clip_count += clipped_this_frame;
                asp_log_warn("musicplayer", "CLIPPING: %d samples clipped in frame %u (max=%d min=%d)",
                            clipped_this_frame, g_frame_count, g_max_sample, g_min_sample);
            }

            // Log format on first successful decode
            if (!g_format_logged) {
                asp_log_info("musicplayer", "Format: %d Hz, %d ch, %d kbps",
                            info.hz, info.channels, info.bitrate_kbps);
                // Only reconfigure I2S if sample rate is different
                if ((uint32_t)info.hz != g_sample_rate) {
                    asp_log_info("musicplayer", "Changing sample rate from %u to %d",
                                (unsigned)g_sample_rate, info.hz);
                    asp_audio_stop();
                    asp_audio_set_rate(info.hz);
                    asp_audio_start();
                }
                g_sample_rate = info.hz;
                g_format_logged = true;
                // Reset debug counters for new file
                g_clip_count = 0;
                g_frame_count = 0;
                g_max_sample = 0;
                g_min_sample = 0;
            }

            // Write to audio output (samples * channels * bytes per sample)
            // Note: volume attenuation is now done in minimp3's mp3d_scale_pcm()
            size_t bytes = samples * info.channels * sizeof(int16_t);
            asp_audio_write(pcm_buffer, bytes, 500);
            g_samples_written += samples;
        } else if (info.frame_bytes == 0) {
            // Need more data or invalid frame, try to refill
            if (fill_buffer() == 0) {
                g_song_finished = true;
                g_playing = false;
                asp_log_info("musicplayer", "Song finished (no more data, total clips=%u max=%d min=%d)",
                            g_clip_count, g_max_sample, g_min_sample);
                break;
            }
        }
    }
    g_thread_in_decode = false;
}

// Start playing a new file (called from decoder thread)
static void start_new_file(const char* path) {
    // Close any existing file
    if (g_current_file) {
        fclose(g_current_file);
        g_current_file = NULL;
    }

    // Open new file
    g_current_file = fopen(path, "rb");
    if (!g_current_file) {
        asp_log_error("musicplayer", "Failed to open: %s", path);
        g_playing = false;
        return;
    }

    // Reset decoder state
    mp3dec_init(g_mp3_decoder);
    buffer_pos = 0;
    buffer_len = 0;

    g_samples_written = 0;
    g_song_finished = false;
    g_format_logged = false;  // Reset for new file
    g_fill_count = 0;  // Reset debug counter
    g_warned_buffer_low = false;  // Reset buffer warning flag
    g_paused = false;
    g_playing = true;

    // Force I2S channel reset: stop, reconfigure, start
    asp_audio_stop();
    asp_audio_set_rate(44100);  // Will be updated when we decode first frame
    asp_audio_start();

    // Enable amplifier and set volume
    asp_audio_set_amplifier(true);
    music_player_state_t* state = music_player_get_state();
    asp_audio_set_volume((float)state->volume);

    asp_log_info("musicplayer", "Playing: %s", path);
}

// Decoder thread main function
static void* decoder_thread_func(void* arg) {
    (void)arg;
    asp_log_info("musicplayer", "Decoder thread started");

    while (!g_thread_should_stop) {
        // Check for new file to play
        if (g_new_file_pending) {
            g_new_file_pending = false;
            start_new_file(g_pending_path);
        }

        // Decode if playing
        if (g_playing && !g_paused) {
            decode_loop();
        } else {
            // Sleep when idle
            asp_plugin_delay_ms(20);
        }
    }

    asp_log_info("musicplayer", "Decoder thread exiting");
    g_thread_running = false;
    return NULL;
}

int audio_init(void) {
    // Guard against double initialization
    if (g_audio_initialized) {
        asp_log_warn("musicplayer", "Audio already initialized, cleaning up first");
        audio_cleanup();
    }

    asp_log_info("musicplayer", "Allocating audio buffers...");

    // Allocate buffers on heap (PSRAM) - PCM buffer is static in SRAM
    read_buffer = (uint8_t*)malloc(READ_BUFFER_SIZE);
    if (!read_buffer) {
        asp_log_error("musicplayer", "Failed to allocate read_buffer (%d bytes)", READ_BUFFER_SIZE);
        return -1;
    }

    g_mp3_decoder = (mp3dec_t*)malloc(sizeof(mp3dec_t));
    if (!g_mp3_decoder) {
        asp_log_error("musicplayer", "Failed to allocate mp3_decoder (%d bytes)", (int)sizeof(mp3dec_t));
        free(read_buffer);
        read_buffer = NULL;
        return -1;
    }

    asp_log_info("musicplayer", "Buffers allocated, creating decoder thread...");

    // Initialize MP3 decoder
    mp3dec_init(g_mp3_decoder);

    // Create decoder thread with larger stack
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, DECODER_STACK_SIZE);

    g_thread_should_stop = false;
    g_thread_in_decode = false;
    int err = pthread_create(&decoder_thread, &attr, decoder_thread_func, NULL);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        asp_log_error("musicplayer", "Failed to create decoder thread: %d (need %d bytes stack)",
                     err, DECODER_STACK_SIZE);
        // Thread creation failed - heap may be corrupted or out of memory
        // Try to free our buffers, but be aware this might fail
        free(read_buffer);
        free(g_mp3_decoder);
        read_buffer = NULL;
        g_mp3_decoder = NULL;
        return -1;
    }

    g_thread_running = true;
    g_audio_initialized = true;

    // Note: Don't call asp_audio_start() - I2S channel is already enabled by BSP

    asp_log_info("musicplayer", "Audio initialized (32KB decoder stack)");
    return 0;
}

void audio_cleanup(void) {
    if (!g_audio_initialized) {
        return;
    }

    asp_log_info("musicplayer", "Audio cleanup starting...");

    // First, stop playback to get thread out of decode_loop
    g_playing = false;
    g_paused = false;
    g_new_file_pending = false;

    // Wait for thread to exit decode_loop (it checks g_playing each frame)
    // asp_audio_write has 500ms timeout, so wait up to 600ms
    for (int i = 0; i < 30 && g_thread_in_decode; i++) {
        asp_plugin_delay_ms(20);
    }
    if (g_thread_in_decode) {
        asp_log_warn("musicplayer", "Thread still in decode loop after 600ms");
    }

    // Now signal thread to fully stop
    g_thread_should_stop = true;

    // Wait for decoder thread to exit
    if (g_thread_running) {
        asp_log_info("musicplayer", "Waiting for decoder thread to exit...");
        // Poll for thread exit (up to 2 seconds)
        for (int i = 0; i < 100 && g_thread_running; i++) {
            asp_plugin_delay_ms(20);
        }
        if (g_thread_running) {
            asp_log_warn("musicplayer", "Decoder thread did not set exit flag within timeout");
        }
        // pthread_join will block until thread actually exits
        pthread_join(decoder_thread, NULL);
        asp_log_info("musicplayer", "pthread_join completed");
    }
    g_thread_running = false;

    // Small delay to let system reclaim thread resources
    asp_plugin_delay_ms(50);

    // Close file if open
    if (g_current_file) {
        fclose(g_current_file);
        g_current_file = NULL;
    }

    // Mute output
    asp_audio_set_amplifier(false);

    // Free heap buffers (PCM buffer is static)
    if (read_buffer) {
        free(read_buffer);
        read_buffer = NULL;
    }
    if (g_mp3_decoder) {
        free(g_mp3_decoder);
        g_mp3_decoder = NULL;
    }

    // Reset all state for clean plugin reload
    g_playing = false;
    g_paused = false;
    g_song_finished = false;
    g_samples_written = 0;
    g_sample_rate = 44100;
    g_format_logged = false;
    g_fill_count = 0;
    g_warned_buffer_low = false;
    g_thread_in_decode = false;
    buffer_pos = 0;
    buffer_len = 0;
    g_new_file_pending = false;
    g_thread_should_stop = false;
    memset(g_pending_path, 0, sizeof(g_pending_path));
    // Reset debug counters
    g_clip_count = 0;
    g_frame_count = 0;
    g_max_sample = 0;
    g_min_sample = 0;

    g_audio_initialized = false;
    asp_log_info("musicplayer", "Audio cleanup complete");
}

void audio_play_file(const char* path) {
    // Stop current playback - decoder thread will close file
    g_playing = false;
    g_paused = false;

    // Wait a bit for decoder thread to notice and stop
    asp_plugin_delay_ms(30);

    // Signal decoder thread to play new file
    strncpy(g_pending_path, path, sizeof(g_pending_path) - 1);
    g_pending_path[sizeof(g_pending_path) - 1] = '\0';
    g_new_file_pending = true;
}

void audio_stop(void) {
    g_playing = false;
    g_paused = false;
    g_new_file_pending = false;
    asp_audio_set_amplifier(false);
}

void audio_pause(void) {
    g_paused = true;
    asp_audio_set_amplifier(false);
}

void audio_resume(void) {
    if (g_playing) {
        g_paused = false;
        asp_audio_set_amplifier(true);
    }
}

void audio_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    asp_audio_set_volume((float)volume);
}

bool audio_is_finished(void) {
    return g_song_finished;
}

uint32_t audio_get_position_ms(void) {
    if (g_sample_rate == 0) return 0;
    return (uint32_t)((g_samples_written * 1000ULL) / g_sample_rate);
}

bool audio_process(void) {
    // Processing is now done in decoder thread
    // Just return current state
    return g_playing && !g_song_finished;
}

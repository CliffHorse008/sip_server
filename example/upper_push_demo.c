#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "sipserver/config.h"
#include "sipserver/sip_embed.h"

/* G711A 在 8kHz 下每 20ms 对应 160 个采样。 */
#define G711A_SAMPLES_PER_PACKET 160U

static sip_embed_service_t *g_service = NULL;

/* 示例程序自己的配置，公共 SIP/RTP 配置之外的字段不进入库 API。 */
typedef struct {
    app_config_t app;
    double video_fps;
    char video_path[256];
    char g711a_path[256];
} sample_config_t;

/* 一个 H264 Access Unit 在裸码流中的偏移与长度。 */
typedef struct {
    unsigned char *data;
    size_t size;
} h264_access_unit_t;

typedef int (*h264_access_unit_output_fn)(const uint8_t *access_unit,
                                          size_t access_unit_size,
                                          void *user_data);

/*
 * 可移植的 H264 Annex-B 分片转 Access Unit 状态。
 * 板端使用时按实时收到的 H264 码流片段反复调用 demo_h264_input_stream_fragment()，
 * 回调输出的 access_unit 可直接传给 sip_embed_service_push_video_frame()。
 */
typedef struct {
    unsigned char *pending_stream;
    size_t pending_stream_size;
    size_t pending_stream_capacity;
    unsigned char *current_au;
    size_t current_au_size;
    size_t current_au_capacity;
    int have_current_au;
    int current_au_has_vcl;
    size_t nalu_count;
} h264_access_unit_stream_t;

/* 示例推流线程需要的测试媒体缓存。 */
typedef struct {
    h264_access_unit_t *video_frames;
    size_t video_frame_count;
    size_t video_nalu_count;
    unsigned char *audio_blob;
    size_t audio_blob_size;
    pthread_t audio_thread;
    pthread_t video_thread;
    int audio_thread_running;
    int video_thread_running;
} sample_demo_media_t;

typedef struct {
    sample_demo_media_t *demo_media;
    int failed;
} h264_access_unit_collect_ctx_t;

/* 示例宿主上下文，包含嵌入式服务实例与演示媒体状态。 */
typedef struct {
    const sample_config_t *config;
    sip_embed_service_t *service;
    sample_demo_media_t demo_media;
    pthread_mutex_t media_log_mutex;
    long long media_log_window_start_ms;
    unsigned int media_log_packet_count;
    size_t media_log_total_bytes;
    unsigned int media_log_audio_packets;
    unsigned int media_log_video_packets;
} sample_host_t;

/* 捕获退出信号，让主循环和工作线程尽快收敛。 */
static void handle_signal(int signum)
{
    (void) signum;
    if (g_service != NULL) {
        sip_embed_service_stop(g_service);
    }
}

/* 用纳秒粒度休眠，并在收到退出信号时提前结束。 */
static void sleep_ns(long long nanoseconds)
{
    struct timespec ts;

    if (nanoseconds <= 0) {
        return;
    }

    ts.tv_sec = (time_t) (nanoseconds / 1000000000LL);
    ts.tv_nsec = (long) (nanoseconds % 1000000000LL);
    while (nanosleep(&ts, &ts) != 0 &&
           (g_service == NULL || !sip_embed_service_stop_requested(g_service))) {
    }
}

static long long monotonic_time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000LL + (long long) tv.tv_usec / 1000LL;
}

static int parse_double_value(const char *value, double *out)
{
    char *end = NULL;
    double parsed = strtod(value, &end);

    if (value[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }

    *out = parsed;
    return 0;
}

static void sample_config_set_defaults(sample_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config_set_defaults(&config->app);
    config->video_fps = 30.0;
    snprintf(config->video_path, sizeof(config->video_path), "test_media/video.h264");
    snprintf(config->g711a_path, sizeof(config->g711a_path), "test_media/audio.g711a");
}

static void sample_config_print_usage(FILE *stream, const char *program_name)
{
    config_print_usage(stream, program_name);
    fprintf(stream,
            "Demo options:\n"
            "  --video-fps <fps>     H264 pacing FPS, default 30.0\n"
            "  --video-file <path>   H264 Annex-B bitstream path\n"
            "  --g711a-file <path>   G711A raw payload path\n");
}

static int sample_config_parse(sample_config_t *config, int argc, char **argv)
{
    char **app_argv;
    int app_argc = 1;
    int index;
    int parse_rc;

    sample_config_set_defaults(config);

    app_argv = (char **) malloc(sizeof(*app_argv) * (size_t) argc);
    if (app_argv == NULL) {
        fprintf(stderr, "failed to allocate argv buffer\n");
        return -1;
    }

    app_argv[0] = argv[0];
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0) {
            sample_config_print_usage(stdout, argv[0]);
            free(app_argv);
            return 1;
        }

        if (strcmp(arg, "--video-fps") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "missing value for option: %s\n", arg);
                free(app_argv);
                return -1;
            }
            if (parse_double_value(argv[++index], &config->video_fps) != 0 || config->video_fps <= 0.0) {
                fprintf(stderr, "invalid video fps\n");
                free(app_argv);
                return -1;
            }
            continue;
        }

        if (strcmp(arg, "--video-file") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "missing value for option: %s\n", arg);
                free(app_argv);
                return -1;
            }
            snprintf(config->video_path, sizeof(config->video_path), "%s", argv[++index]);
            continue;
        }

        if (strcmp(arg, "--g711a-file") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "missing value for option: %s\n", arg);
                free(app_argv);
                return -1;
            }
            snprintf(config->g711a_path, sizeof(config->g711a_path), "%s", argv[++index]);
            continue;
        }

        app_argv[app_argc++] = argv[index];
        if (index + 1 < argc) {
            app_argv[app_argc++] = argv[++index];
        }
    }

    parse_rc = config_parse(&config->app, app_argc, app_argv);
    free(app_argv);
    return parse_rc;
}

/* 一次性把整个文件读入内存，用于测试媒体加载。 */
static int read_entire_file(const char *path, unsigned char **buffer, size_t *size)
{
    FILE *stream;
    long file_size;
    unsigned char *blob;

    *buffer = NULL;
    *size = 0;

    stream = fopen(path, "rb");
    if (stream == NULL) {
        return -1;
    }

    if (fseek(stream, 0, SEEK_END) != 0) {
        fclose(stream);
        return -1;
    }

    file_size = ftell(stream);
    if (file_size < 0) {
        fclose(stream);
        return -1;
    }

    if (fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return -1;
    }

    blob = (unsigned char *) malloc((size_t) file_size);
    if (blob == NULL) {
        fclose(stream);
        return -1;
    }

    if (file_size > 0 && fread(blob, 1, (size_t) file_size, stream) != (size_t) file_size) {
        free(blob);
        fclose(stream);
        return -1;
    }

    fclose(stream);
    *buffer = blob;
    *size = (size_t) file_size;
    return 0;
}

/* 在 Annex-B H264 码流中查找起始码位置。 */
static int find_start_code(const unsigned char *data,
                           size_t size,
                           size_t offset,
                           size_t *start_offset,
                           size_t *code_size)
{
    size_t index;

    if (size < 4 || offset >= size - 3) {
        return 0;
    }

    for (index = offset; index + 3 < size; ++index) {
        if (data[index] == 0x00 && data[index + 1] == 0x00) {
            if (data[index + 2] == 0x01) {
                *start_offset = index;
                *code_size = 3;
                return 1;
            }
            if (index + 4 < size && data[index + 2] == 0x00 && data[index + 3] == 0x01) {
                *start_offset = index;
                *code_size = 4;
                return 1;
            }
        }
    }

    return 0;
}

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t bit_offset;
} h264_bit_reader_t;

static int h264_read_bit(h264_bit_reader_t *reader, unsigned int *bit)
{
    size_t byte_offset;
    unsigned int shift;

    if (reader == NULL || bit == NULL || reader->bit_offset >= reader->size * 8U) {
        return -1;
    }

    byte_offset = reader->bit_offset / 8U;
    shift = (unsigned int) (7U - (reader->bit_offset % 8U));
    *bit = (unsigned int) ((reader->data[byte_offset] >> shift) & 0x01U);
    ++reader->bit_offset;
    return 0;
}

static int h264_read_ue(h264_bit_reader_t *reader, unsigned int *value)
{
    unsigned int leading_zero_bits = 0U;
    unsigned int suffix = 0U;
    unsigned int bit;
    unsigned int index;

    if (reader == NULL || value == NULL) {
        return -1;
    }

    while (1) {
        if (h264_read_bit(reader, &bit) != 0) {
            return -1;
        }
        if (bit != 0U) {
            break;
        }
        ++leading_zero_bits;
        if (leading_zero_bits > 31U) {
            return -1;
        }
    }

    for (index = 0U; index < leading_zero_bits; ++index) {
        if (h264_read_bit(reader, &bit) != 0) {
            return -1;
        }
        suffix = (suffix << 1U) | bit;
    }

    *value = ((1U << leading_zero_bits) - 1U) + suffix;
    return 0;
}

static size_t h264_ebsp_to_rbsp(const unsigned char *ebsp,
                                size_t ebsp_size,
                                unsigned char *rbsp,
                                size_t rbsp_capacity)
{
    size_t src_index;
    size_t dst_index = 0U;
    unsigned int zero_count = 0U;

    for (src_index = 0U; src_index < ebsp_size; ++src_index) {
        unsigned char byte = ebsp[src_index];

        if (zero_count >= 2U && byte == 0x03U) {
            zero_count = 0U;
            continue;
        }

        if (dst_index >= rbsp_capacity) {
            return 0U;
        }
        rbsp[dst_index++] = byte;

        if (byte == 0x00U) {
            ++zero_count;
        } else {
            zero_count = 0U;
        }
    }

    return dst_index;
}

static int h264_nalu_is_vcl(unsigned char nal_type)
{
    return nal_type >= 1U && nal_type <= 5U;
}

static int h264_nalu_first_mb_is_zero(const unsigned char *nalu, size_t nalu_size, int *is_zero)
{
    unsigned char *rbsp;
    size_t rbsp_size;
    h264_bit_reader_t reader;
    unsigned int first_mb_in_slice;

    if (nalu == NULL || nalu_size <= 1U || is_zero == NULL) {
        return -1;
    }

    rbsp = (unsigned char *) malloc(nalu_size - 1U);
    if (rbsp == NULL) {
        return -1;
    }

    rbsp_size = h264_ebsp_to_rbsp(nalu + 1U, nalu_size - 1U, rbsp, nalu_size - 1U);
    if (rbsp_size == 0U) {
        free(rbsp);
        return -1;
    }

    reader.data = rbsp;
    reader.size = rbsp_size;
    reader.bit_offset = 0U;
    if (h264_read_ue(&reader, &first_mb_in_slice) != 0) {
        free(rbsp);
        return -1;
    }

    free(rbsp);
    *is_zero = first_mb_in_slice == 0U;
    return 0;
}

static void h264_access_unit_stream_init(h264_access_unit_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
}

static void h264_access_unit_stream_cleanup(h264_access_unit_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    free(stream->pending_stream);
    free(stream->current_au);
    memset(stream, 0, sizeof(*stream));
}

static int h264_reserve_buffer(unsigned char **buffer,
                               size_t *capacity,
                               size_t current_size,
                               size_t additional_size)
{
    unsigned char *grown;
    size_t required_size;
    size_t new_capacity;

    if (buffer == NULL || capacity == NULL) {
        return -1;
    }
    if (additional_size > ((size_t) -1) - current_size) {
        return -1;
    }

    required_size = current_size + additional_size;
    if (required_size <= *capacity) {
        return 0;
    }

    new_capacity = *capacity > 0U ? *capacity : 256U;
    while (new_capacity < required_size) {
        if (new_capacity > ((size_t) -1) / 2U) {
            new_capacity = required_size;
            break;
        }
        new_capacity *= 2U;
    }

    grown = (unsigned char *) realloc(*buffer, new_capacity);
    if (grown == NULL) {
        return -1;
    }

    *buffer = grown;
    *capacity = new_capacity;
    return 0;
}

static int h264_access_unit_stream_append_pending(h264_access_unit_stream_t *stream,
                                                  const uint8_t *fragment,
                                                  size_t fragment_size)
{
    if (stream == NULL) {
        return -1;
    }
    if (fragment_size == 0U) {
        return 0;
    }
    if (fragment == NULL) {
        return -1;
    }

    if (h264_reserve_buffer(&stream->pending_stream,
                            &stream->pending_stream_capacity,
                            stream->pending_stream_size,
                            fragment_size) != 0) {
        return -1;
    }

    memcpy(stream->pending_stream + stream->pending_stream_size, fragment, fragment_size);
    stream->pending_stream_size += fragment_size;
    return 0;
}

static void h264_access_unit_stream_discard_pending_prefix(h264_access_unit_stream_t *stream,
                                                           size_t keep_offset)
{
    if (stream == NULL || keep_offset == 0U) {
        return;
    }

    if (keep_offset >= stream->pending_stream_size) {
        stream->pending_stream_size = 0U;
        return;
    }

    memmove(stream->pending_stream,
            stream->pending_stream + keep_offset,
            stream->pending_stream_size - keep_offset);
    stream->pending_stream_size -= keep_offset;
}

static int h264_access_unit_stream_emit_current(h264_access_unit_stream_t *stream,
                                                h264_access_unit_output_fn output,
                                                void *user_data)
{
    int rc;

    if (stream == NULL) {
        return -1;
    }
    if (!stream->have_current_au || stream->current_au_size == 0U) {
        stream->have_current_au = 0;
        stream->current_au_has_vcl = 0;
        stream->current_au_size = 0U;
        return 0;
    }
    if (output == NULL) {
        return -1;
    }

    rc = output(stream->current_au, stream->current_au_size, user_data);
    stream->have_current_au = 0;
    stream->current_au_has_vcl = 0;
    stream->current_au_size = 0U;
    return rc;
}

static int h264_access_unit_stream_append_nalu(h264_access_unit_stream_t *stream,
                                               const unsigned char *nalu,
                                               size_t nalu_size)
{
    static const unsigned char start_code[4] = {0x00U, 0x00U, 0x00U, 0x01U};

    if (stream == NULL || nalu == NULL || nalu_size == 0U) {
        return -1;
    }

    if (h264_reserve_buffer(&stream->current_au,
                            &stream->current_au_capacity,
                            stream->current_au_size,
                            sizeof(start_code) + nalu_size) != 0) {
        return -1;
    }

    memcpy(stream->current_au + stream->current_au_size, start_code, sizeof(start_code));
    stream->current_au_size += sizeof(start_code);
    memcpy(stream->current_au + stream->current_au_size, nalu, nalu_size);
    stream->current_au_size += nalu_size;
    stream->have_current_au = 1;
    return 0;
}

static int h264_access_unit_stream_input_nalu(h264_access_unit_stream_t *stream,
                                              const unsigned char *nalu,
                                              size_t nalu_size,
                                              h264_access_unit_output_fn output,
                                              void *user_data)
{
    unsigned char nal_type;
    int starts_new_au = 0;

    if (stream == NULL || nalu == NULL || nalu_size == 0U) {
        return -1;
    }

    nal_type = (unsigned char) (nalu[0] & 0x1FU);
    if (stream->have_current_au && stream->current_au_has_vcl) {
        if (nal_type == 9U || nal_type == 6U || nal_type == 7U || nal_type == 8U ||
            (nal_type >= 10U && nal_type <= 18U)) {
            starts_new_au = 1;
        } else if (h264_nalu_is_vcl(nal_type)) {
            int first_mb_is_zero = 0;

            if (h264_nalu_first_mb_is_zero(nalu, nalu_size, &first_mb_is_zero) == 0 &&
                first_mb_is_zero) {
                starts_new_au = 1;
            }
        }
    }

    if (starts_new_au && h264_access_unit_stream_emit_current(stream, output, user_data) != 0) {
        return -1;
    }

    if (h264_access_unit_stream_append_nalu(stream, nalu, nalu_size) != 0) {
        return -1;
    }

    if (h264_nalu_is_vcl(nal_type)) {
        stream->current_au_has_vcl = 1;
    }
    ++stream->nalu_count;
    return 0;
}

static int h264_access_unit_stream_process_pending(h264_access_unit_stream_t *stream,
                                                   int flush,
                                                   h264_access_unit_output_fn output,
                                                   void *user_data)
{
    while (stream->pending_stream_size > 0U) {
        size_t start_offset;
        size_t code_size;
        size_t nal_offset;
        size_t next_offset;
        size_t next_code_size;
        size_t nal_end;
        int has_next;

        if (!find_start_code(stream->pending_stream,
                             stream->pending_stream_size,
                             0U,
                             &start_offset,
                             &code_size)) {
            if (stream->pending_stream_size > 3U) {
                h264_access_unit_stream_discard_pending_prefix(stream,
                                                               stream->pending_stream_size - 3U);
            }
            return 0;
        }

        if (start_offset > 0U) {
            h264_access_unit_stream_discard_pending_prefix(stream, start_offset);
            continue;
        }

        nal_offset = code_size;
        if (nal_offset >= stream->pending_stream_size) {
            return 0;
        }

        has_next = find_start_code(stream->pending_stream,
                                   stream->pending_stream_size,
                                   nal_offset,
                                   &next_offset,
                                   &next_code_size);
        (void) next_code_size;
        if (!has_next && !flush) {
            return 0;
        }

        nal_end = has_next ? next_offset : stream->pending_stream_size;
        while (nal_end > nal_offset && stream->pending_stream[nal_end - 1U] == 0x00U) {
            --nal_end;
        }

        if (nal_end > nal_offset &&
            h264_access_unit_stream_input_nalu(stream,
                                               stream->pending_stream + nal_offset,
                                               nal_end - nal_offset,
                                               output,
                                               user_data) != 0) {
            return -1;
        }

        if (has_next) {
            h264_access_unit_stream_discard_pending_prefix(stream, next_offset);
        } else {
            stream->pending_stream_size = 0U;
        }
    }

    if (flush && h264_access_unit_stream_emit_current(stream, output, user_data) != 0) {
        return -1;
    }

    return 0;
}

static int demo_h264_input_stream_fragment(h264_access_unit_stream_t *stream,
                                           const uint8_t *fragment,
                                           size_t fragment_size,
                                           int flush,
                                           h264_access_unit_output_fn output,
                                           void *user_data)
{
    if (stream == NULL || output == NULL) {
        return -1;
    }

    if (h264_access_unit_stream_append_pending(stream, fragment, fragment_size) != 0) {
        return -1;
    }

    return h264_access_unit_stream_process_pending(stream, flush, output, user_data);
}

/* 追加一个 Access Unit 的缓存副本。 */
static int demo_append_h264_access_unit(sample_demo_media_t *demo_media,
                                        const uint8_t *access_unit,
                                        size_t access_unit_size)
{
    h264_access_unit_t *grown;
    size_t new_count;
    unsigned char *copy;

    if (access_unit == NULL || access_unit_size == 0) {
        return 0;
    }

    copy = (unsigned char *) malloc(access_unit_size);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, access_unit, access_unit_size);

    new_count = demo_media->video_frame_count + 1;
    grown = (h264_access_unit_t *) realloc(demo_media->video_frames, new_count * sizeof(*grown));
    if (grown == NULL) {
        free(copy);
        return -1;
    }

    demo_media->video_frames = grown;
    demo_media->video_frames[demo_media->video_frame_count].data = copy;
    demo_media->video_frames[demo_media->video_frame_count].size = access_unit_size;
    demo_media->video_frame_count = new_count;
    return 0;
}

static void demo_free_h264_access_units(sample_demo_media_t *demo_media)
{
    size_t index;

    if (demo_media == NULL) {
        return;
    }

    for (index = 0; index < demo_media->video_frame_count; ++index) {
        free(demo_media->video_frames[index].data);
        demo_media->video_frames[index].data = NULL;
        demo_media->video_frames[index].size = 0;
    }
    free(demo_media->video_frames);
    demo_media->video_frames = NULL;
    demo_media->video_frame_count = 0;
    demo_media->video_nalu_count = 0;
}

static int demo_collect_h264_access_unit(const uint8_t *access_unit,
                                         size_t access_unit_size,
                                         void *user_data)
{
    h264_access_unit_collect_ctx_t *ctx = (h264_access_unit_collect_ctx_t *) user_data;

    if (ctx == NULL || ctx->demo_media == NULL) {
        return -1;
    }
    if (demo_append_h264_access_unit(ctx->demo_media, access_unit, access_unit_size) != 0) {
        ctx->failed = 1;
        return -1;
    }

    return 0;
}

/*
 * 演示：把文件内存按小块喂给 demo_h264_input_stream_fragment()。
 * 实际板端可保留 h264_access_unit_stream_t 和 demo_h264_input_stream_fragment()，
 * 每次收到编码器/网络给出的 H264 片段就调用一次；回调出的 AU 直接送
 * sip_embed_service_push_video_frame()。
 */
static int demo_build_h264_access_units(sample_demo_media_t *demo_media,
                                        const unsigned char *video_blob,
                                        size_t video_blob_size)
{
    h264_access_unit_stream_t stream;
    h264_access_unit_collect_ctx_t collect_ctx;
    size_t offset = 0U;
    enum { DEMO_H264_INPUT_CHUNK_SIZE = 137U };

    if (demo_media == NULL || video_blob == NULL || video_blob_size == 0U) {
        return -1;
    }

    h264_access_unit_stream_init(&stream);
    collect_ctx.demo_media = demo_media;
    collect_ctx.failed = 0;
    demo_media->video_nalu_count = 0;

    while (offset < video_blob_size) {
        size_t remaining = video_blob_size - offset;
        size_t chunk_size = remaining > DEMO_H264_INPUT_CHUNK_SIZE
                                ? DEMO_H264_INPUT_CHUNK_SIZE
                                : remaining;

        if (demo_h264_input_stream_fragment(&stream,
                                            video_blob + offset,
                                            chunk_size,
                                            0,
                                            demo_collect_h264_access_unit,
                                            &collect_ctx) != 0) {
            h264_access_unit_stream_cleanup(&stream);
            return -1;
        }
        offset += chunk_size;
    }

    if (demo_h264_input_stream_fragment(&stream,
                                        NULL,
                                        0U,
                                        1,
                                        demo_collect_h264_access_unit,
                                        &collect_ctx) != 0) {
        h264_access_unit_stream_cleanup(&stream);
        return -1;
    }

    demo_media->video_nalu_count = stream.nalu_count;
    h264_access_unit_stream_cleanup(&stream);
    return !collect_ctx.failed && demo_media->video_frame_count > 0U ? 0 : -1;
}

/* 判断一个 Access Unit 内是否包含 IDR，用于拥塞恢复后的重新起播。 */
static int demo_h264_access_unit_has_idr(const unsigned char *data, size_t size)
{
    size_t start_offset;
    size_t code_size;

    if (data == NULL || size == 0) {
        return 0;
    }

    if (!find_start_code(data, size, 0, &start_offset, &code_size)) {
        return (data[0] & 0x1FU) == 5U;
    }

    while (1) {
        size_t nal_offset = start_offset + code_size;
        size_t next_offset;
        size_t next_code_size;

        if (nal_offset < size && (data[nal_offset] & 0x1FU) == 5U) {
            return 1;
        }

        if (!find_start_code(data, size, nal_offset, &next_offset, &next_code_size)) {
            break;
        }

        start_offset = next_offset;
        code_size = next_code_size;
    }

    return 0;
}

/* 由示例线程向当前会话投递一帧音频。 */
static int sample_host_push_audio_frame(sample_host_t *host,
                                        const uint8_t *payload,
                                        size_t payload_size,
                                        uint32_t rtp_timestamp)
{
    return sip_embed_service_push_audio_frame(host->service, payload, payload_size, rtp_timestamp);
}

/* 由示例线程向当前会话投递一帧视频。 */
static int sample_host_push_video_frame(sample_host_t *host,
                                        const uint8_t *access_unit,
                                        size_t access_unit_size,
                                        uint32_t rtp_timestamp)
{
    return sip_embed_service_push_video_frame(host->service, access_unit, access_unit_size, rtp_timestamp);
}

/* 预加载本地测试媒体，供“上层主动推流”示例线程循环发送。 */
static int sample_demo_media_load(sample_host_t *host)
{
    sample_demo_media_t *demo_media = &host->demo_media;
    unsigned char *video_blob = NULL;
    size_t video_blob_size = 0;

    if (read_entire_file(host->config->video_path, &video_blob, &video_blob_size) == 0) {
        if (demo_build_h264_access_units(demo_media, video_blob, video_blob_size) != 0) {
            fprintf(stderr, "failed to parse H264 access units from %s\n", host->config->video_path);
            demo_free_h264_access_units(demo_media);
        }
        free(video_blob);
    } else {
        fprintf(stderr, "failed to load video file: %s\n", host->config->video_path);
    }

    if (host->config->app.audio_codec == AUDIO_CODEC_G711A) {
        if (read_entire_file(host->config->g711a_path, &demo_media->audio_blob, &demo_media->audio_blob_size) != 0) {
            fprintf(stderr, "failed to load G711A file: %s\n", host->config->g711a_path);
        }
    }

    fprintf(stdout,
            "demo media loaded video_nalus=%zu video_frames=%zu audio_bytes=%zu\n",
            demo_media->video_nalu_count,
            demo_media->video_frame_count,
            demo_media->audio_blob_size);
    return 0;
}

/* 示例音频线程：按 20ms 节奏循环推送 G711A 数据。 */
static void *sample_demo_audio_thread_main(void *opaque)
{
    sample_host_t *host = (sample_host_t *) opaque;
    sample_demo_media_t *demo_media = &host->demo_media;
    unsigned int generation = 0;
    size_t offset = 0;
    uint32_t timestamp = 0;
    int announced = 0;
    int paused_for_backpressure = 0;

    while (!sip_embed_service_stop_requested(host->service)) {
        int stream_active;

        sip_embed_service_get_stream_state(host->service, &stream_active, &generation);
        if (!stream_active || demo_media->audio_blob == NULL || demo_media->audio_blob_size == 0) {
            announced = 0;
            paused_for_backpressure = 0;
            sleep_ns(20000000LL);
            continue;
        }

        if (!announced) {
            fprintf(stdout, "demo audio push started\n");
            announced = 1;
        }

        if (offset >= demo_media->audio_blob_size) {
            offset = 0;
        }

        if (sip_embed_service_audio_backpressure_high(host->service)) {
            if (!paused_for_backpressure) {
                fprintf(stdout, "demo audio push paused due to KCP backlog\n");
                paused_for_backpressure = 1;
            }
            sleep_ns(20000000LL);
            continue;
        }

        if (paused_for_backpressure) {
            fprintf(stdout, "demo audio push resumed after KCP backlog recovered\n");
            paused_for_backpressure = 0;
        }

        {
            size_t remaining = demo_media->audio_blob_size - offset;
            size_t chunk = remaining >= G711A_SAMPLES_PER_PACKET ? G711A_SAMPLES_PER_PACKET : remaining;

            if (chunk == 0) {
                offset = 0;
                continue;
            }

            if (sample_host_push_audio_frame(host, demo_media->audio_blob + offset, chunk, timestamp) == 0) {
                offset += chunk;
                timestamp += (uint32_t) chunk;
            }
        }

        /* 如果会话已经切换，则从头开始重新送帧。 */
        sleep_ns(20000000LL);

        {
            int current_stream_active;
            unsigned int current_generation;

            sip_embed_service_get_stream_state(host->service, &current_stream_active, &current_generation);
            (void) current_stream_active;
            if (current_generation != generation) {
                offset = 0;
                timestamp = 0;
                announced = 0;
                paused_for_backpressure = 0;
            }
        }
    }

    return NULL;
}

/* 示例视频线程：按配置帧率循环推送 H264 Access Unit。 */
static void *sample_demo_video_thread_main(void *opaque)
{
    sample_host_t *host = (sample_host_t *) opaque;
    sample_demo_media_t *demo_media = &host->demo_media;
    unsigned int generation = 0;
    size_t frame_index = 0;
    uint32_t timestamp = 0;
    long long frame_interval_ns = (long long) (1000000000.0 / host->config->video_fps);
    uint32_t timestamp_step = (uint32_t) (90000.0 / host->config->video_fps);
    int announced = 0;
    int waiting_for_idr = 0;

    if (frame_interval_ns <= 0) {
        frame_interval_ns = 33333333LL;
    }
    if (timestamp_step == 0) {
        timestamp_step = 3000;
    }

    while (!sip_embed_service_stop_requested(host->service)) {
        int stream_active;

        sip_embed_service_get_stream_state(host->service, &stream_active, &generation);
        if (!stream_active || demo_media->video_frames == NULL || demo_media->video_frame_count == 0) {
            announced = 0;
            waiting_for_idr = 0;
            sleep_ns(10000000LL);
            continue;
        }

        if (!announced) {
            fprintf(stdout, "demo video push started\n");
            announced = 1;
        }

        if (frame_index >= demo_media->video_frame_count) {
            frame_index = 0;
        }

        {
            const h264_access_unit_t *access_unit = &demo_media->video_frames[frame_index];
            int backlog_high = sip_embed_service_video_backpressure_high(host->service);
            int is_idr = demo_h264_access_unit_has_idr(access_unit->data, access_unit->size);

            if (backlog_high && !waiting_for_idr) {
                fprintf(stdout, "demo video push paused due to KCP backlog, wait for next IDR\n");
                waiting_for_idr = 1;
            }

            if (waiting_for_idr) {
                if (backlog_high || !is_idr) {
                    ++frame_index;
                    timestamp += timestamp_step;
                    sleep_ns(frame_interval_ns);
                    continue;
                }

                fprintf(stdout, "demo video push resumed on IDR frame\n");
                waiting_for_idr = 0;
            }

            if (sample_host_push_video_frame(host, access_unit->data, access_unit->size, timestamp) == 0) {
                ++frame_index;
                timestamp += timestamp_step;
            }
        }

        /* streamer 发生切换后，视频时间线也要同步重置。 */
        sleep_ns(frame_interval_ns);

        {
            int current_stream_active;
            unsigned int current_generation;

            sip_embed_service_get_stream_state(host->service, &current_stream_active, &current_generation);
            (void) current_stream_active;
            if (current_generation != generation) {
                frame_index = 0;
                timestamp = 0;
                announced = 0;
                waiting_for_idr = 0;
            }
        }
    }

    return NULL;
}

/* 启动示例音视频推流线程。 */
static int sample_demo_media_start(sample_host_t *host)
{
    sample_demo_media_t *demo_media = &host->demo_media;

    if (demo_media->audio_blob != NULL && demo_media->audio_blob_size > 0) {
        if (pthread_create(&demo_media->audio_thread, NULL, sample_demo_audio_thread_main, host) != 0) {
            perror("pthread_create(audio-push)");
            return -1;
        }
        demo_media->audio_thread_running = 1;
    }

    if (demo_media->video_frames != NULL && demo_media->video_frame_count > 0) {
        if (pthread_create(&demo_media->video_thread, NULL, sample_demo_video_thread_main, host) != 0) {
            perror("pthread_create(video-push)");
            sip_embed_service_stop(host->service);
            if (demo_media->audio_thread_running) {
                pthread_join(demo_media->audio_thread, NULL);
                demo_media->audio_thread_running = 0;
            }
            return -1;
        }
        demo_media->video_thread_running = 1;
    }

    return 0;
}

/* 等待示例推流线程退出。 */
static void sample_demo_media_stop(sample_host_t *host)
{
    sample_demo_media_t *demo_media = &host->demo_media;

    if (demo_media->audio_thread_running) {
        pthread_join(demo_media->audio_thread, NULL);
        demo_media->audio_thread_running = 0;
    }
    if (demo_media->video_thread_running) {
        pthread_join(demo_media->video_thread, NULL);
        demo_media->video_thread_running = 0;
    }
}

static void sample_host_on_media(const streamer_rtp_packet_t *packet, void *user_data)
{
    sample_host_t *host = (sample_host_t *) user_data;
    long long now_ms = monotonic_time_ms();
    int should_log = 0;
    unsigned int packet_count = 0;
    size_t total_bytes = 0;
    unsigned int audio_packets = 0;
    unsigned int video_packets = 0;

    pthread_mutex_lock(&host->media_log_mutex);
    if (host->media_log_window_start_ms == 0) {
        host->media_log_window_start_ms = now_ms;
    }

    ++host->media_log_packet_count;
    host->media_log_total_bytes += packet->payload_size;
    if (packet->kind == STREAMER_MEDIA_AUDIO) {
        ++host->media_log_audio_packets;
    } else {
        ++host->media_log_video_packets;
    }

    if (now_ms - host->media_log_window_start_ms >= 1000) {
        should_log = 1;
        packet_count = host->media_log_packet_count;
        total_bytes = host->media_log_total_bytes;
        audio_packets = host->media_log_audio_packets;
        video_packets = host->media_log_video_packets;
        host->media_log_window_start_ms = now_ms;
        host->media_log_packet_count = 0;
        host->media_log_total_bytes = 0;
        host->media_log_audio_packets = 0;
        host->media_log_video_packets = 0;
    }
    pthread_mutex_unlock(&host->media_log_mutex);

    if (should_log) {
        fprintf(stdout,
                "media rx packets=%u bytes=%zu audio=%u video=%u last_kind=%s pt=%u seq=%u ts=%u size=%zu from=%s:%u\n",
                packet_count,
                total_bytes,
                audio_packets,
                video_packets,
                packet->kind == STREAMER_MEDIA_AUDIO ? "audio" : "video",
                packet->payload_type,
                packet->sequence,
                packet->timestamp,
                packet->payload_size,
                packet->source_ip != NULL ? packet->source_ip : "-",
                packet->source_port);
    }
}

static void sample_host_on_signal(const sip_signal_event_t *event, void *user_data)
{
    sample_host_t *host = (sample_host_t *) user_data;

    (void) host;
    if (event->type == SIP_SIGNAL_RESPONSE_SENT) {
        fprintf(stdout,
                "app signal response call_id=%s status=%d reason=%s\n",
                event->call_id != NULL ? event->call_id : "-",
                event->status_code,
                event->reason_phrase != NULL ? event->reason_phrase : "-");
        return;
    }

    fprintf(stdout,
            "app signal type=%d call_id=%s method=%s from=%s to=%s source=%s:%u\n",
            event->type,
            event->call_id != NULL ? event->call_id : "-",
            event->method != NULL ? event->method : "-",
            event->from != NULL ? event->from : "-",
            event->to != NULL ? event->to : "-",
            event->source_ip != NULL ? event->source_ip : "-",
            event->source_port);
}

/* 释放 SDK 上下文持有的线程、媒体缓存与互斥锁。 */
static void sample_host_destroy(sample_host_t *host)
{
    sample_demo_media_t *demo_media = &host->demo_media;

    sample_demo_media_stop(host);
    pthread_mutex_destroy(&host->media_log_mutex);
    demo_free_h264_access_units(demo_media);
    free(demo_media->audio_blob);
}

/* 初始化示例宿主，并启动本地演示推流线程。 */
static int sample_host_init(sample_host_t *host, const sample_config_t *config, sip_embed_service_t *service)
{
    memset(host, 0, sizeof(*host));
    host->config = config;
    host->service = service;

    if (pthread_mutex_init(&host->media_log_mutex, NULL) != 0) {
        return -1;
    }

    if (sample_demo_media_load(host) != 0) {
        sample_host_destroy(host);
        return -1;
    }

    if (sample_demo_media_start(host) != 0) {
        sample_host_destroy(host);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    sample_config_t config;
    sample_host_t host;
    sip_embed_service_t *service;
    sip_embed_callbacks_t callbacks;
    int parse_rc;
    int run_rc;

    srand((unsigned int) time(NULL));

    /* 先解析命令行；帮助信息与参数错误在这里直接返回。 */
    parse_rc = sample_config_parse(&config, argc, argv);
    if (parse_rc > 0) {
        return 0;
    }
    if (parse_rc < 0) {
        sample_config_print_usage(stderr, argv[0]);
        return 1;
    }

    fprintf(stdout, "libsip buildin: %s\n", sip_embed_build_time());

    service = sip_embed_service_create(&config.app);
    if (service == NULL) {
        return 1;
    }
    g_service = service;

    if (sample_host_init(&host, &config, service) != 0) {
        g_service = NULL;
        sip_embed_service_destroy(service);
        return 1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_signal = sample_host_on_signal;
    callbacks.on_media = sample_host_on_media;
    callbacks.user_data = &host;
    sip_embed_service_set_callbacks(service, &callbacks);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stdout,
            "sipserver starting on %s:%u/%s, media=%s, rtp_transport=%s, audio_codec=%s, mode=upper-push-demo\n",
            config.app.bind_ip,
            config.app.sip_port,
            config_sip_transport_name(config.app.sip_transport),
            config.app.media_ip,
            config_rtp_transport_name(config.app.rtp_transport),
            config_audio_codec_name(config.app.audio_codec));

    run_rc = sip_embed_service_run(service);
    sip_embed_service_stop(service);
    sample_host_destroy(&host);
    g_service = NULL;
    sip_embed_service_destroy(service);
    return run_rc;
}

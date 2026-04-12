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

static volatile sig_atomic_t g_stop = 0;

/* 一个 H264 Access Unit 在裸码流中的偏移与长度。 */
typedef struct {
    size_t offset;
    size_t size;
} h264_access_unit_t;

/* 示例推流线程需要的测试媒体缓存。 */
typedef struct {
    unsigned char *video_blob;
    size_t video_blob_size;
    h264_access_unit_t *video_frames;
    size_t video_frame_count;
    unsigned char *audio_blob;
    size_t audio_blob_size;
    pthread_t audio_thread;
    pthread_t video_thread;
    int audio_thread_running;
    int video_thread_running;
} sample_demo_media_t;

/* 示例宿主上下文，包含嵌入式服务实例与演示媒体状态。 */
typedef struct {
    const app_config_t *config;
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
    g_stop = 1;
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
    while (nanosleep(&ts, &ts) != 0 && g_stop == 0) {
    }
}

static long long monotonic_time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000LL + (long long) tv.tv_usec / 1000LL;
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

/* 追加一个 Access Unit 的区间记录。 */
static int demo_append_h264_access_unit(sample_demo_media_t *demo_media, size_t offset, size_t size)
{
    h264_access_unit_t *grown;
    size_t new_count;

    if (size == 0) {
        return 0;
    }

    new_count = demo_media->video_frame_count + 1;
    grown = (h264_access_unit_t *) realloc(demo_media->video_frames, new_count * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }

    demo_media->video_frames = grown;
    demo_media->video_frames[demo_media->video_frame_count].offset = offset;
    demo_media->video_frames[demo_media->video_frame_count].size = size;
    demo_media->video_frame_count = new_count;
    return 0;
}

/* 通过 AUD 或起始 NAL 边界，把裸 H264 拆成逐帧发送的 Access Unit。 */
static int demo_build_h264_access_units(sample_demo_media_t *demo_media)
{
    size_t current_offset = 0;
    size_t search_offset = 0;
    int have_current = 0;

    while (1) {
        size_t start_offset;
        size_t code_size;
        size_t next_offset;
        size_t next_code_size;
        size_t nal_offset;
        unsigned char nal_type;

        if (!find_start_code(demo_media->video_blob,
                             demo_media->video_blob_size,
                             search_offset,
                             &start_offset,
                             &code_size)) {
            break;
        }

        nal_offset = start_offset + code_size;
        if (nal_offset >= demo_media->video_blob_size) {
            break;
        }

        nal_type = (unsigned char) (demo_media->video_blob[nal_offset] & 0x1F);

        if (nal_type == 9) {
            if (have_current && start_offset > current_offset) {
                if (demo_append_h264_access_unit(demo_media, current_offset, start_offset - current_offset) != 0) {
                    return -1;
                }
            }
            current_offset = start_offset;
            have_current = 1;
        } else if (!have_current) {
            current_offset = start_offset;
            have_current = 1;
        }

        if (!find_start_code(demo_media->video_blob,
                             demo_media->video_blob_size,
                             nal_offset,
                             &next_offset,
                             &next_code_size)) {
            (void) next_code_size;
            break;
        }

        search_offset = next_offset;
    }

    if (have_current && current_offset < demo_media->video_blob_size) {
        if (demo_append_h264_access_unit(demo_media,
                                         current_offset,
                                         demo_media->video_blob_size - current_offset) != 0) {
            return -1;
        }
    }

    return demo_media->video_frame_count > 0 ? 0 : -1;
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

    if (read_entire_file(host->config->video_path, &demo_media->video_blob, &demo_media->video_blob_size) == 0) {
        if (demo_build_h264_access_units(demo_media) != 0) {
            fprintf(stderr, "failed to parse H264 access units from %s\n", host->config->video_path);
            free(demo_media->video_blob);
            demo_media->video_blob = NULL;
            demo_media->video_blob_size = 0;
            free(demo_media->video_frames);
            demo_media->video_frames = NULL;
            demo_media->video_frame_count = 0;
        }
    } else {
        fprintf(stderr, "failed to load video file: %s\n", host->config->video_path);
    }

    if (host->config->audio_codec == AUDIO_CODEC_G711A) {
        if (read_entire_file(host->config->g711a_path, &demo_media->audio_blob, &demo_media->audio_blob_size) != 0) {
            fprintf(stderr, "failed to load G711A file: %s\n", host->config->g711a_path);
        }
    }

    fprintf(stdout,
            "demo media loaded video_frames=%zu audio_bytes=%zu\n",
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

    while (g_stop == 0) {
        int stream_active;

        sip_embed_service_get_stream_state(host->service, &stream_active, &generation);
        if (!stream_active || demo_media->audio_blob == NULL || demo_media->audio_blob_size == 0) {
            announced = 0;
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

    if (frame_interval_ns <= 0) {
        frame_interval_ns = 33333333LL;
    }
    if (timestamp_step == 0) {
        timestamp_step = 3000;
    }

    while (g_stop == 0) {
        int stream_active;

        sip_embed_service_get_stream_state(host->service, &stream_active, &generation);
        if (!stream_active || demo_media->video_frames == NULL || demo_media->video_frame_count == 0) {
            announced = 0;
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

            if (sample_host_push_video_frame(host, demo_media->video_blob + access_unit->offset, access_unit->size, timestamp) ==
                0) {
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
            g_stop = 1;
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

/* 释放 SDK 上下文持有的线程、媒体缓存与互斥锁。 */
static void sample_host_destroy(sample_host_t *host)
{
    sample_demo_media_t *demo_media = &host->demo_media;

    sample_demo_media_stop(host);
    pthread_mutex_destroy(&host->media_log_mutex);
    free(demo_media->video_frames);
    free(demo_media->video_blob);
    free(demo_media->audio_blob);
}

/* 初始化示例宿主，并启动本地演示推流线程。 */
static int sample_host_init(sample_host_t *host, const app_config_t *config, sip_embed_service_t *service)
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
    app_config_t config;
    sample_host_t host;
    sip_embed_service_t *service;
    sip_embed_callbacks_t callbacks;
    int parse_rc;
    int run_rc;

    srand((unsigned int) time(NULL));

    /* 先解析命令行；帮助信息与参数错误在这里直接返回。 */
    parse_rc = config_parse(&config, argc, argv);
    if (parse_rc > 0) {
        return 0;
    }
    if (parse_rc < 0) {
        config_print_usage(stderr, argv[0]);
        return 1;
    }

    service = sip_embed_service_create(&config);
    if (service == NULL) {
        return 1;
    }

    if (sample_host_init(&host, &config, service) != 0) {
        sip_embed_service_destroy(service);
        return 1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_media = sample_host_on_media;
    callbacks.user_data = &host;
    sip_embed_service_set_callbacks(service, &callbacks);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stdout,
            "sipserver starting on %s:%u/%s, media=%s, audio_codec=%s, mode=upper-push-demo\n",
            config.bind_ip,
            config.sip_port,
            config_sip_transport_name(config.sip_transport),
            config.media_ip,
            config_audio_codec_name(config.audio_codec));

    run_rc = sip_embed_service_run(service, &g_stop);
    g_stop = 1;
    sample_host_destroy(&host);
    sip_embed_service_destroy(service);
    return run_rc;
}

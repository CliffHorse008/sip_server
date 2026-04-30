#ifndef H264_ACCESS_UNIT_STREAM_H
#define H264_ACCESS_UNIT_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef int (*h264_access_unit_output_fn)(const uint8_t *access_unit,
                                          size_t access_unit_size,
                                          void *user_data);

/*
 * 输入要求：每次传入一个边界完整的 NALU，且不带 Annex-B 起始码。
 * 输出方式：当检测到 AU 边界时，通过回调输出完整 AU，输出格式为 Annex-B。
 */
typedef struct {
    unsigned char *current_au;
    size_t current_au_size;
    size_t current_au_capacity;
    int have_current_au;
    int current_au_has_vcl;
    size_t nalu_count;
} h264_access_unit_stream_t;

void h264_access_unit_stream_init(h264_access_unit_stream_t *stream);
void h264_access_unit_stream_cleanup(h264_access_unit_stream_t *stream);
int h264_access_unit_stream_push_nalu(h264_access_unit_stream_t *stream,
                                      const uint8_t *nalu,
                                      size_t nalu_size,
                                      int flush,
                                      h264_access_unit_output_fn output,
                                      void *user_data);

#endif

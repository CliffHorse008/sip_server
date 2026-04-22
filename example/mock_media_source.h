#ifndef EXAMPLE_MOCK_MEDIA_SOURCE_H
#define EXAMPLE_MOCK_MEDIA_SOURCE_H

#include <stddef.h>
#include <stdint.h>

/* 组装 H264 Access Unit 的演示状态。 */
typedef struct {
    unsigned char *current_au;
    size_t current_au_size;
    size_t current_au_capacity;
    int have_current;
    int current_has_vcl;
    unsigned char *ready_au;
    size_t ready_au_size;
} mock_h264_au_builder_t;

/* 初始化 / 释放演示用 AU builder。 */
void mock_h264_au_builder_init(mock_h264_au_builder_t *builder);
void mock_h264_au_builder_cleanup(mock_h264_au_builder_t *builder);

/*
 * 输入一个 H264 NALU 片段，必要时输出一个完整 Access Unit。
 *
 * 入参可以是：
 * - 带 Annex-B 起始码的单个 NALU 片段
 * - 不带起始码的裸 NALU
 *
 * 输出的 AU 会被规范化成 Annex-B（每个 NALU 前写入 4 字节起始码）。
 * 返回值：
 * - 1: 产生了一个完整 AU，结果通过 out_au / out_au_size 返回
 * - 0: 当前还不足以产出 AU
 * - -1: 输入非法或内存分配失败
 *
 * 注意：out_au 指针只在下一次 input/flush/cleanup 之前有效。
 */
int mock_h264_input_nalu(mock_h264_au_builder_t *builder,
                         const uint8_t *nalu_fragment,
                         size_t nalu_fragment_size,
                         const uint8_t **out_au,
                         size_t *out_au_size);

/*
 * 在输入结束时 flush builder，吐出最后一个 AU。
 * 返回值语义与 mock_h264_input_nalu 相同。
 */
int mock_h264_flush(mock_h264_au_builder_t *builder,
                    const uint8_t **out_au,
                    size_t *out_au_size);

#endif

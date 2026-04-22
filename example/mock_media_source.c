#include "mock_media_source.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t bit_offset;
} h264_bit_reader_t;

static int mock_h264_find_start_code(const unsigned char *data,
                                     size_t size,
                                     size_t offset,
                                     size_t *start_offset,
                                     size_t *code_size)
{
    size_t index;

    if (data == NULL || start_offset == NULL || code_size == NULL || size < 3U || offset >= size) {
        return 0;
    }

    for (index = offset; index + 3U <= size; ++index) {
        if (data[index] == 0x00U && data[index + 1] == 0x00U) {
            if (index + 3U <= size && data[index + 2] == 0x01U) {
                *start_offset = index;
                *code_size = 3U;
                return 1;
            }
            if (index + 4U <= size && data[index + 2] == 0x00U && data[index + 3] == 0x01U) {
                *start_offset = index;
                *code_size = 4U;
                return 1;
            }
        }
    }

    return 0;
}

static int mock_h264_read_bit(h264_bit_reader_t *reader, unsigned int *bit)
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

static int mock_h264_read_ue(h264_bit_reader_t *reader, unsigned int *value)
{
    unsigned int leading_zero_bits = 0;
    unsigned int bit;
    unsigned int suffix = 0;
    unsigned int index;

    if (reader == NULL || value == NULL) {
        return -1;
    }

    while (1) {
        if (mock_h264_read_bit(reader, &bit) != 0) {
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

    for (index = 0; index < leading_zero_bits; ++index) {
        if (mock_h264_read_bit(reader, &bit) != 0) {
            return -1;
        }
        suffix = (suffix << 1U) | bit;
    }

    *value = ((1U << leading_zero_bits) - 1U) + suffix;
    return 0;
}

static size_t mock_h264_ebsp_to_rbsp(const unsigned char *ebsp,
                                     size_t ebsp_size,
                                     unsigned char *rbsp,
                                     size_t rbsp_capacity)
{
    size_t src_index;
    size_t dst_index = 0;
    unsigned int zero_count = 0;

    for (src_index = 0; src_index < ebsp_size; ++src_index) {
        unsigned char byte = ebsp[src_index];

        if (zero_count >= 2U && byte == 0x03U) {
            zero_count = 0;
            continue;
        }

        if (dst_index >= rbsp_capacity) {
            return 0;
        }
        rbsp[dst_index++] = byte;

        if (byte == 0x00U) {
            ++zero_count;
        } else {
            zero_count = 0;
        }
    }

    return dst_index;
}

static int mock_h264_nalu_is_vcl(unsigned char nal_type)
{
    return nal_type >= 1U && nal_type <= 5U;
}

static int mock_h264_nalu_first_mb_is_zero(const unsigned char *nalu, size_t nalu_size, int *is_zero)
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

    rbsp_size = mock_h264_ebsp_to_rbsp(nalu + 1U, nalu_size - 1U, rbsp, nalu_size - 1U);
    if (rbsp_size == 0U) {
        free(rbsp);
        return -1;
    }

    reader.data = rbsp;
    reader.size = rbsp_size;
    reader.bit_offset = 0;
    if (mock_h264_read_ue(&reader, &first_mb_in_slice) != 0) {
        free(rbsp);
        return -1;
    }

    free(rbsp);
    *is_zero = first_mb_in_slice == 0U;
    return 0;
}

static void mock_h264_clear_ready(mock_h264_au_builder_t *builder)
{
    free(builder->ready_au);
    builder->ready_au = NULL;
    builder->ready_au_size = 0;
}

static int mock_h264_reserve(mock_h264_au_builder_t *builder, size_t additional_size)
{
    unsigned char *grown;
    size_t required_size;
    size_t new_capacity;

    if (builder == NULL) {
        return -1;
    }

    required_size = builder->current_au_size + additional_size;
    if (required_size <= builder->current_au_capacity) {
        return 0;
    }

    new_capacity = builder->current_au_capacity > 0U ? builder->current_au_capacity : 256U;
    while (new_capacity < required_size) {
        if (new_capacity > ((size_t) -1) / 2U) {
            new_capacity = required_size;
            break;
        }
        new_capacity *= 2U;
    }

    grown = (unsigned char *) realloc(builder->current_au, new_capacity);
    if (grown == NULL) {
        return -1;
    }

    builder->current_au = grown;
    builder->current_au_capacity = new_capacity;
    return 0;
}

static int mock_h264_append_nalu(mock_h264_au_builder_t *builder, const unsigned char *nalu, size_t nalu_size)
{
    static const unsigned char start_code[4] = {0x00U, 0x00U, 0x00U, 0x01U};

    if (builder == NULL || nalu == NULL || nalu_size == 0U) {
        return -1;
    }

    if (mock_h264_reserve(builder, 4U + nalu_size) != 0) {
        return -1;
    }

    memcpy(builder->current_au + builder->current_au_size, start_code, sizeof(start_code));
    builder->current_au_size += sizeof(start_code);
    memcpy(builder->current_au + builder->current_au_size, nalu, nalu_size);
    builder->current_au_size += nalu_size;
    builder->have_current = 1;
    return 0;
}

static int mock_h264_finalize_current(mock_h264_au_builder_t *builder)
{
    if (builder == NULL) {
        return -1;
    }

    if (!builder->have_current || builder->current_au_size == 0U) {
        builder->have_current = 0;
        builder->current_has_vcl = 0;
        builder->current_au_size = 0;
        return 0;
    }

    mock_h264_clear_ready(builder);
    builder->ready_au = builder->current_au;
    builder->ready_au_size = builder->current_au_size;
    builder->current_au = NULL;
    builder->current_au_size = 0;
    builder->current_au_capacity = 0;
    builder->have_current = 0;
    builder->current_has_vcl = 0;
    return 0;
}

void mock_h264_au_builder_init(mock_h264_au_builder_t *builder)
{
    if (builder == NULL) {
        return;
    }

    memset(builder, 0, sizeof(*builder));
}

void mock_h264_au_builder_cleanup(mock_h264_au_builder_t *builder)
{
    if (builder == NULL) {
        return;
    }

    free(builder->current_au);
    builder->current_au = NULL;
    builder->current_au_size = 0;
    builder->current_au_capacity = 0;
    builder->have_current = 0;
    builder->current_has_vcl = 0;
    mock_h264_clear_ready(builder);
}

int mock_h264_input_nalu(mock_h264_au_builder_t *builder,
                         const uint8_t *nalu_fragment,
                         size_t nalu_fragment_size,
                         const uint8_t **out_au,
                         size_t *out_au_size)
{
    size_t start_offset = 0;
    size_t code_size = 0;
    size_t nal_offset = 0;
    size_t nal_size;
    const unsigned char *nalu;
    unsigned char nal_type;
    int starts_new_au = 0;

    if (out_au != NULL) {
        *out_au = NULL;
    }
    if (out_au_size != NULL) {
        *out_au_size = 0;
    }

    if (builder == NULL || nalu_fragment == NULL || nalu_fragment_size == 0U) {
        return -1;
    }

    mock_h264_clear_ready(builder);

    if (mock_h264_find_start_code(nalu_fragment,
                                  nalu_fragment_size,
                                  0,
                                  &start_offset,
                                  &code_size) &&
        start_offset == 0U) {
        size_t nal_end = nalu_fragment_size;

        nal_offset = code_size;
        while (nal_end > nal_offset && nalu_fragment[nal_end - 1U] == 0x00U) {
            --nal_end;
        }
        nal_size = nal_end - nal_offset;
    } else {
        nal_offset = 0U;
        nal_size = nalu_fragment_size;
    }

    if (nal_size == 0U) {
        return -1;
    }

    nalu = nalu_fragment + nal_offset;
    nal_type = (unsigned char) (nalu[0] & 0x1FU);

    if (builder->have_current && builder->current_has_vcl) {
        if (nal_type == 9U || nal_type == 6U || nal_type == 7U || nal_type == 8U ||
            (nal_type >= 10U && nal_type <= 18U)) {
            starts_new_au = 1;
        } else if (mock_h264_nalu_is_vcl(nal_type)) {
            int first_mb_is_zero = 0;

            if (mock_h264_nalu_first_mb_is_zero(nalu, nal_size, &first_mb_is_zero) == 0 &&
                first_mb_is_zero) {
                starts_new_au = 1;
            }
        }
    }

    if (starts_new_au && mock_h264_finalize_current(builder) != 0) {
        return -1;
    }

    if (mock_h264_append_nalu(builder, nalu, nal_size) != 0) {
        return -1;
    }

    if (mock_h264_nalu_is_vcl(nal_type)) {
        builder->current_has_vcl = 1;
    }

    if (builder->ready_au != NULL) {
        if (out_au != NULL) {
            *out_au = builder->ready_au;
        }
        if (out_au_size != NULL) {
            *out_au_size = builder->ready_au_size;
        }
        return 1;
    }

    return 0;
}

int mock_h264_flush(mock_h264_au_builder_t *builder, const uint8_t **out_au, size_t *out_au_size)
{
    if (out_au != NULL) {
        *out_au = NULL;
    }
    if (out_au_size != NULL) {
        *out_au_size = 0;
    }

    if (builder == NULL) {
        return -1;
    }

    mock_h264_clear_ready(builder);
    if (!builder->have_current || builder->current_au_size == 0U) {
        return 0;
    }

    if (mock_h264_finalize_current(builder) != 0) {
        return -1;
    }

    if (out_au != NULL) {
        *out_au = builder->ready_au;
    }
    if (out_au_size != NULL) {
        *out_au_size = builder->ready_au_size;
    }
    return 1;
}

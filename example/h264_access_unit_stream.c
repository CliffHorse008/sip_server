#include "h264_access_unit_stream.h"

#include <stdlib.h>
#include <string.h>

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

void h264_access_unit_stream_init(h264_access_unit_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
}

void h264_access_unit_stream_cleanup(h264_access_unit_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

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

int h264_access_unit_stream_push_nalu(h264_access_unit_stream_t *stream,
                                      const uint8_t *nalu,
                                      size_t nalu_size,
                                      int flush,
                                      h264_access_unit_output_fn output,
                                      void *user_data)
{
    if (stream == NULL || output == NULL) {
        return -1;
    }

    if (nalu_size > 0U) {
        if (nalu == NULL) {
            return -1;
        }
        if (h264_access_unit_stream_input_nalu(stream, nalu, nalu_size, output, user_data) != 0) {
            return -1;
        }
    }

    if (flush && h264_access_unit_stream_emit_current(stream, output, user_data) != 0) {
        return -1;
    }

    return 0;
}

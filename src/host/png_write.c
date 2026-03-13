#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void png_write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static uint32_t png_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }

    return ~crc;
}

static uint32_t png_adler32(const uint8_t *data, size_t size) {
    uint32_t a = 1u;
    uint32_t b = 0u;

    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }

    return (b << 16) | a;
}

static bool png_write_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t size) {
    uint8_t header[8];
    uint8_t crc_bytes[4];
    uint32_t crc;

    png_write_be32(header, size);
    memcpy(header + 4, type, 4);

    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        return false;
    }
    if (size != 0 && fwrite(data, 1, size, file) != size) {
        return false;
    }

    crc = png_crc32((const uint8_t *)type, 4);
    if (size != 0) {
        uint32_t data_crc = png_crc32(data, size);
        /* Fold the data into the CRC by continuing the same bitwise algorithm. */
        crc = 0xffffffffu;
        crc = png_crc32((const uint8_t *)type, 4);
        {
            size_t total_size = (size_t)4 + size;
            uint8_t *chunk_bytes = (uint8_t *)malloc(total_size);
            if (chunk_bytes == NULL) {
                return false;
            }
            memcpy(chunk_bytes, type, 4);
            memcpy(chunk_bytes + 4, data, size);
            crc = png_crc32(chunk_bytes, total_size);
            free(chunk_bytes);
        }
        (void)data_crc;
    }

    png_write_be32(crc_bytes, crc);
    return fwrite(crc_bytes, 1, sizeof(crc_bytes), file) == sizeof(crc_bytes);
}

bool host_write_png_gray8(
    const char *path,
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    enum { ZLIB_HEADER_SIZE = 2, ZLIB_TRAILER_SIZE = 4, DEFLATE_MAX_BLOCK = 65535 };
    static const uint8_t png_signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    FILE *file = NULL;
    uint8_t ihdr[13];
    uint8_t *filtered = NULL;
    uint8_t *zlib_stream = NULL;
    size_t filtered_size;
    size_t block_count;
    size_t zlib_size;
    size_t filtered_offset = 0;
    size_t zlib_offset = 0;
    bool ok = false;

    if (path == NULL || pixels == NULL || width <= 0 || height <= 0 || stride < width) {
        return false;
    }

    filtered_size = (size_t)height * (size_t)(width + 1);
    filtered = (uint8_t *)malloc(filtered_size);
    if (filtered == NULL) {
        goto cleanup;
    }

    for (int y = 0; y < height; ++y) {
        filtered[y * (size_t)(width + 1)] = 0;
        memcpy(
            &filtered[y * (size_t)(width + 1) + 1],
            pixels + (size_t)y * (size_t)stride,
            (size_t)width
        );
    }

    block_count = (filtered_size + DEFLATE_MAX_BLOCK - 1) / DEFLATE_MAX_BLOCK;
    zlib_size = ZLIB_HEADER_SIZE + filtered_size + block_count * 5 + ZLIB_TRAILER_SIZE;
    zlib_stream = (uint8_t *)malloc(zlib_size);
    if (zlib_stream == NULL) {
        goto cleanup;
    }

    zlib_stream[zlib_offset++] = 0x78;
    zlib_stream[zlib_offset++] = 0x01;

    while (filtered_offset < filtered_size) {
        size_t remaining = filtered_size - filtered_offset;
        uint16_t block_size = (uint16_t)(remaining > DEFLATE_MAX_BLOCK ? DEFLATE_MAX_BLOCK : remaining);
        uint16_t nlen = (uint16_t)~block_size;
        bool final_block = (filtered_offset + block_size) == filtered_size;

        zlib_stream[zlib_offset++] = final_block ? 0x01u : 0x00u;
        zlib_stream[zlib_offset++] = (uint8_t)(block_size & 0xffu);
        zlib_stream[zlib_offset++] = (uint8_t)(block_size >> 8);
        zlib_stream[zlib_offset++] = (uint8_t)(nlen & 0xffu);
        zlib_stream[zlib_offset++] = (uint8_t)(nlen >> 8);
        memcpy(&zlib_stream[zlib_offset], &filtered[filtered_offset], block_size);
        zlib_offset += block_size;
        filtered_offset += block_size;
    }

    png_write_be32(&zlib_stream[zlib_offset], png_adler32(filtered, filtered_size));
    zlib_offset += ZLIB_TRAILER_SIZE;

    file = fopen(path, "wb");
    if (file == NULL) {
        goto cleanup;
    }

    if (fwrite(png_signature, 1, sizeof(png_signature), file) != sizeof(png_signature)) {
        goto cleanup;
    }

    png_write_be32(&ihdr[0], (uint32_t)width);
    png_write_be32(&ihdr[4], (uint32_t)height);
    ihdr[8] = 8;
    ihdr[9] = 0;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    if (!png_write_chunk(file, "IHDR", ihdr, sizeof(ihdr))) {
        goto cleanup;
    }
    if (!png_write_chunk(file, "IDAT", zlib_stream, (uint32_t)zlib_offset)) {
        goto cleanup;
    }
    if (!png_write_chunk(file, "IEND", NULL, 0)) {
        goto cleanup;
    }

    ok = true;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    free(zlib_stream);
    free(filtered);
    return ok;
}

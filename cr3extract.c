#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// Updated JpegInfo to use size_t instead of long
typedef struct {
    size_t start;  // Changed from long to size_t
    size_t end;    // Changed from long to size_t
    size_t size;   // Changed from long to size_t
} JpegInfo;

#define STREAM_BUFFER_SIZE 4096

// Global flags
int g_minimize_exif = 0;
int g_extract_all = 0;
int g_extract_index = -1;
char *g_output_filename = NULL;

// Function prototypes
int find_all_jpegs(FILE *file, JpegInfo **jpegs, int *count);
uint16_t read16le(const unsigned char *data, size_t offset, size_t dataSize);
uint32_t read32le(const unsigned char *data, size_t offset, size_t dataSize);
int findBox_streaming(FILE *f, size_t start, size_t end, const char *target,
                      unsigned char **result, size_t *resultSize);
int extractCr3Exif_streaming(FILE *f, size_t fileSize, unsigned char **exifSegment, size_t *exifSize, int verbose);
int minimizeExifData(unsigned char **exifSegment, size_t *exifSize);
int insertExifIntoJpeg(const unsigned char *jpegData, size_t jpegSize,
                       unsigned char *exifSegment, size_t exifSize,
                       unsigned char **outputData, size_t *outputSize);
int extract_largest_jpeg(const char *cr3_path, const char *output_path, int to_stdout, int verbose);
int extract_all_jpegs(const char *cr3_path, int verbose);
int extract_specific_jpeg(const char *cr3_path, int jpeg_index, int to_stdout, int verbose);
char* generate_output_filename(const char* source);
char* generate_output_filename_all(const char* source, int index);
void print_usage(const char *progname);

// print_usage (unchanged)
void print_usage(const char *progname) {
    printf("Usage: %s <infile> [-] [-v] [-m] [-j all|1|2|3] [-o outfile] [-h]\n", progname);
    printf("Options:\n");
    printf("  (no -j) : Extract largest JPEG preview unaltered (no EXIF changes) to file or stdout\n");
    printf("  -       : Output to stdout (allowed in default mode and -j 1|2|3)\n");
    printf("  -v      : Verbose output\n");
    printf("  -m      : Minimize EXIF data (applies only with -j options)\n");
    printf("  -j all  : Extract first 3 JPEG segments with full/minimized EXIF (stdout not allowed)\n");
    printf("  -j 1    : Extract 1st JPEG segment with full/minimized EXIF (stdout allowed)\n");
    printf("  -j 2    : Extract 2nd JPEG segment with full/minimized EXIF (stdout allowed)\n");
    printf("  -j 3    : Extract 3rd JPEG segment with full/minimized EXIF (stdout allowed)\n");
    printf("  -o FILENAME : Specify output file name. In default mode or -j 1|2|3, FILENAME is used exactly.\n");
    printf("                In -j all mode, FILENAME is used as a base name with an index appended.\n");
    printf("  -h      : Print this help message and exit\n");
}

// Updated find_all_jpegs with size_t
int find_all_jpegs(FILE *file, JpegInfo **jpegs, int *count) {
    const size_t BUFFER_SIZE = 4096;
    unsigned char buffer[BUFFER_SIZE];
    size_t bytes_read;
    size_t file_pos = 0;
    int capacity = 10;
    *count = 0;
    *jpegs = malloc(capacity * sizeof(JpegInfo));
    if (!*jpegs) {
        perror("Failed to allocate memory for JPEG array");
        return -1;
    }
    size_t start = (size_t)-1;
    unsigned char last_byte = 0;
    int has_last_byte = 0;
    rewind(file);
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (has_last_byte && bytes_read > 0) {
            if (last_byte == 0xFF && buffer[0] == 0xD8)
                start = file_pos - 1;
            if (start != (size_t)-1 && last_byte == 0xFF && buffer[0] == 0xD9) {
                if (*count >= capacity) {
                    capacity *= 2;
                    JpegInfo *temp = realloc(*jpegs, capacity * sizeof(JpegInfo));
                    if (!temp) {
                        perror("Failed to realloc JPEG array");
                        free(*jpegs);
                        return -1;
                    }
                    *jpegs = temp;
                }
                (*jpegs)[*count].start = start;
                (*jpegs)[*count].end = file_pos + 1;
                (*jpegs)[*count].size = (*jpegs)[*count].end - (*jpegs)[*count].start;
                (*count)++;
                start = (size_t)-1;
            }
        }
        for (size_t i = 0; i < bytes_read - 1; i++) {
            if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8)
                start = file_pos + i;
            if (start != (size_t)-1 && buffer[i] == 0xFF && buffer[i + 1] == 0xD9) {
                if (*count >= capacity) {
                    capacity *= 2;
                    JpegInfo *temp = realloc(*jpegs, capacity * sizeof(JpegInfo));
                    if (!temp) {
                        perror("Failed to realloc JPEG array");
                        free(*jpegs);
                        return -1;
                    }
                    *jpegs = temp;
                }
                (*jpegs)[*count].start = start;
                (*jpegs)[*count].end = file_pos + i + 2;
                (*jpegs)[*count].size = (*jpegs)[*count].end - (*jpegs)[*count].start;
                (*count)++;
                start = (size_t)-1;
            }
        }
        if (bytes_read > 0) {
            last_byte = buffer[bytes_read - 1];
            has_last_byte = 1;
        } else {
            has_last_byte = 0;
        }
        file_pos += bytes_read;
    }
    if (ferror(file)) {
        perror("Error reading input file during JPEG search");
        free(*jpegs);
        *jpegs = NULL;
        *count = 0;
        return -1;
    }
    rewind(file);
    return 0;
}

// Endian Helpers (unchanged)
uint16_t read16le(const unsigned char *data, size_t offset, size_t dataSize) {
    if (offset + 1 >= dataSize) return 0;
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

uint32_t read32le(const unsigned char *data, size_t offset, size_t dataSize) {
    if (offset + 3 >= dataSize) return 0;
    return ((uint32_t)data[offset]) | ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}

// findBox_streaming (unchanged)
int findBox_streaming(FILE *f, size_t start, size_t end, const char *target,
                      unsigned char **result, size_t *resultSize) {
    size_t pos = start;
    while (pos + 8 <= end) {
        if (fseek(f, pos, SEEK_SET) != 0) {
            fprintf(stderr, "fseek failed at pos %zu\n", pos);
            return 0;
        }
        unsigned char header[16];
        if (fread(header, 1, 8, f) != 8) break;
        uint32_t size32 = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
        char boxType[5];
        memcpy(boxType, header + 4, 4);
        boxType[4] = '\0';
        uint64_t boxSize = size32;
        size_t headerSize = 8;
        if (size32 == 1) {
            if (fread(header + 8, 1, 8, f) != 8) break;
            headerSize = 16;
            boxSize = ((uint64_t)header[8] << 56) | ((uint64_t)header[9] << 48) |
                      ((uint64_t)header[10] << 40) | ((uint64_t)header[11] << 32) |
                      ((uint64_t)header[12] << 24) | ((uint64_t)header[13] << 16) |
                      ((uint64_t)header[14] << 8) | (uint64_t)header[15];
        }
        if (boxSize < headerSize) {
            fprintf(stderr, "Invalid box size at position %zu\n", pos);
            return 0;
        }
        size_t boxEnd = pos + boxSize;
        if (boxEnd > end) {
            fprintf(stderr, "Box at position %zu extends beyond file bounds.\n", pos);
            return 0;
        }
        if (strcmp(boxType, target) == 0) {
            size_t contentSize = boxSize - headerSize;
            *result = (unsigned char *)malloc(contentSize);
            if (!*result) {
                fprintf(stderr, "Memory allocation failed in findBox_streaming\n");
                return 0;
            }
            if (fseek(f, pos + headerSize, SEEK_SET) != 0) {
                free(*result);
                return 0;
            }
            if (fread(*result, 1, contentSize, f) != contentSize) {
                free(*result);
                return 0;
            }
            *resultSize = contentSize;
            return 1;
        }
        pos = boxEnd;
    }
    return 0;
}

// extractCr3Exif_streaming (unchanged)
int extractCr3Exif_streaming(FILE *f, size_t fileSize, unsigned char **exifSegment, size_t *exifSize, int verbose) {
    unsigned char *moovBox = NULL;
    size_t moovSize = 0;
    if (!findBox_streaming(f, 0, fileSize, "moov", &moovBox, &moovSize)) {
        if (verbose) fprintf(stderr, "No 'moov' box found in CR3 file.\n");
        return 0;
    }
    size_t pos = 0;
    unsigned char *uuidBox = NULL;
    size_t uuidSize = 0;
    while (pos + 8 <= moovSize) {
        uint32_t size32 = (moovBox[pos] << 24) | (moovBox[pos+1] << 16) |
                          (moovBox[pos+2] << 8) | moovBox[pos+3];
        char type[5];
        memcpy(type, moovBox + pos + 4, 4);
        type[4] = '\0';
        uint64_t boxSize = size32;
        size_t headerSize = 8;
        if (size32 == 1) {
            if (pos + 16 > moovSize) break;
            headerSize = 16;
            boxSize = ((uint64_t)moovBox[pos+8] << 56) | ((uint64_t)moovBox[pos+9] << 48) |
                      ((uint64_t)moovBox[pos+10] << 40) | ((uint64_t)moovBox[pos+11] << 32) |
                      ((uint64_t)moovBox[pos+12] << 24) | ((uint64_t)moovBox[pos+13] << 16) |
                      ((uint64_t)moovBox[pos+14] << 8) | (uint64_t)moovBox[pos+15];
        }
        if (boxSize < headerSize) break;
        if (pos + boxSize > moovSize) break;
        if (strcmp(type, "uuid") == 0) {
            uuidSize = boxSize - headerSize;
            uuidBox = (unsigned char *)malloc(uuidSize);
            if (!uuidBox) {
                fprintf(stderr, "Memory allocation failed for uuidBox\n");
                free(moovBox);
                return 0;
            }
            memcpy(uuidBox, moovBox + pos + headerSize, uuidSize);
            break;
        }
        pos += boxSize;
    }
    free(moovBox);
    if (!uuidBox) {
        if (verbose) fprintf(stderr, "No 'uuid' box found in 'moov' box.\n");
        return 0;
    }
    pos = 0;
    int found = 0;
    while (pos < uuidSize - 4) {
        if (memcmp(uuidBox + pos, "II", 2) == 0) {
            uint16_t marker = read16le(uuidBox, pos + 2, uuidSize);
            if (marker == 42) {
                found = 1;
                break;
            }
        }
        pos++;
    }
    if (!found) {
        if (verbose) fprintf(stderr, "No valid TIFF header found in 'uuid' box.\n");
        free(uuidBox);
        return 0;
    }
    size_t tiffDataSize = uuidSize - pos;
    unsigned char *tiffData = (unsigned char *)malloc(tiffDataSize);
    if (!tiffData) {
        fprintf(stderr, "Memory allocation failed for TIFF data\n");
        free(uuidBox);
        return 0;
    }
    memcpy(tiffData, uuidBox + pos, tiffDataSize);
    free(uuidBox);
    const char exifHeader[6] = {'E','x','i','f',0,0};
    *exifSize = 6 + tiffDataSize;
    *exifSegment = (unsigned char *)malloc(*exifSize);
    if (!*exifSegment) {
        fprintf(stderr, "Memory allocation failed for EXIF segment\n");
        free(tiffData);
        return 0;
    }
    memcpy(*exifSegment, exifHeader, 6);
    memcpy(*exifSegment + 6, tiffData, tiffDataSize);
    free(tiffData);
    return 1;
}

// minimizeExifData (unchanged)
int minimizeExifData(unsigned char **exifSegment, size_t *exifSize) {
    const char *exifHeader = "Exif\0\0";
    if (*exifSize < 10 || memcmp(*exifSegment, exifHeader, 6) != 0) {
        fprintf(stderr, "Not a valid EXIF segment.\n");
        return 0;
    }
    size_t tiffStart = 6;
    if (tiffStart + 8 > *exifSize) return 0;
    uint32_t ifd0RelOffset = read32le(*exifSegment, tiffStart + 4, *exifSize);
    size_t ifd0Offset = tiffStart + ifd0RelOffset;
    if (ifd0Offset + 2 > *exifSize) return 0;
    uint16_t count = read16le(*exifSegment, ifd0Offset, *exifSize);
    size_t ifd0EntriesStart = ifd0Offset + 2;
    size_t ifd0EntriesSize = count * 12;
    if (ifd0EntriesStart + ifd0EntriesSize + 4 > *exifSize) return 0;

    uint16_t allowed[] = { 0x010F, 0x0110, 0x0132, 0x829A, 0x829D, 0x8827, 0x920A, 0x0112 };
    size_t allowedCount = sizeof(allowed) / sizeof(allowed[0]);

    unsigned char *filteredEntries = (unsigned char *)malloc(ifd0EntriesSize);
    if (!filteredEntries) {
        fprintf(stderr, "Memory allocation failed in minimizeExifData\n");
        return 0;
    }
    size_t newEntryCount = 0;
    for (size_t i = 0; i < count; i++) {
        size_t entryOffset = ifd0EntriesStart + i * 12;
        if (entryOffset + 12 > *exifSize) break;
        uint16_t tag = read16le(*exifSegment, entryOffset, *exifSize);
        int keep = 0;
        for (size_t j = 0; j < allowedCount; j++) {
            if (tag == allowed[j]) {
                keep = 1;
                break;
            }
        }
        if (keep) {
            memcpy(filteredEntries + newEntryCount * 12, *exifSegment + entryOffset, 12);
            newEntryCount++;
        }
    }
    size_t newIFD0Size = 2 + newEntryCount * 12 + 4;
    unsigned char *newIFD0 = (unsigned char *)malloc(newIFD0Size);
    if (!newIFD0) {
        fprintf(stderr, "Memory allocation failed for new IFD0 block.\n");
        free(filteredEntries);
        return 0;
    }
    newIFD0[0] = newEntryCount & 0xFF;
    newIFD0[1] = (newEntryCount >> 8) & 0xFF;
    memcpy(newIFD0 + 2, filteredEntries, newEntryCount * 12);
    memset(newIFD0 + 2 + newEntryCount * 12, 0, 4);
    free(filteredEntries);
    size_t newExifSize = ifd0Offset + newIFD0Size;
    unsigned char *newExif = (unsigned char *)malloc(newExifSize);
    if (!newExif) {
        fprintf(stderr, "Memory allocation failed for new EXIF segment\n");
        free(newIFD0);
        return 0;
    }
    memcpy(newExif, *exifSegment, ifd0Offset);
    memcpy(newExif + ifd0Offset, newIFD0, newIFD0Size);
    free(newIFD0);
    free(*exifSegment);
    *exifSegment = newExif;
    *exifSize = newExifSize;
    return 1;
}

// insertExifIntoJpeg (corrected from previous fix)
int insertExifIntoJpeg(const unsigned char *jpegData, size_t jpegSize,
                       unsigned char *exifSegment, size_t exifSize,
                       unsigned char **outputData, size_t *outputSize) {
    if (jpegSize < 2 || jpegData[0] != 0xFF || jpegData[1] != 0xD8) {
        fprintf(stderr, "Extracted data is not a valid JPEG.\n");
        return 0;
    }
    *outputSize = 2 + 4 + exifSize + (jpegSize - 2);
    *outputData = (unsigned char *)malloc(*outputSize);
    if (!*outputData) {
        fprintf(stderr, "Memory allocation failed for output JPEG.\n");
        return 0;
    }
    size_t pos = 0;
    memcpy(*outputData, jpegData, 2);
    pos += 2;
    (*outputData)[pos++] = 0xFF;
    (*outputData)[pos++] = 0xE1;
    uint16_t segLength = exifSize + 2;
    (*outputData)[pos++] = (segLength >> 8) & 0xFF;
    (*outputData)[pos++] = segLength & 0xFF;
    memcpy(*outputData + pos, exifSegment, exifSize);
    pos += exifSize;
    memcpy(*outputData + pos, jpegData + 2, jpegSize - 2);
    return 1;
}

// Updated extract_largest_jpeg with size_t (unchanged in terms of JPEG selection)
int extract_largest_jpeg(const char *cr3_path, const char *output_path, int to_stdout, int verbose) {
    FILE *cr3_file = fopen(cr3_path, "rb");
    if (!cr3_file) {
        perror("Failed to open CR3 file");
        return -1;
    }

    JpegInfo *jpegs = NULL;
    int jpeg_count = 0;
    if (find_all_jpegs(cr3_file, &jpegs, &jpeg_count) != 0) {
        fprintf(stderr, "Failed to scan for JPEG previews in CR3 file.\n");
        fclose(cr3_file);
        if (jpegs) free(jpegs);
        return -1;
    }

    if (jpeg_count == 0) {
        fprintf(stderr, "No JPEG previews found in CR3 file: %s\n", cr3_path);
        fclose(cr3_file);
        if (jpegs) free(jpegs);
        return -1;
    }

    int largest_idx = 0;
    for (int i = 1; i < jpeg_count; i++) {
        if (jpegs[i].size > jpegs[largest_idx].size)
            largest_idx = i;
    }

    size_t jpeg_start_offset = jpegs[largest_idx].start;
    size_t jpeg_size = jpegs[largest_idx].size;

    if (fseek(cr3_file, jpeg_start_offset, SEEK_SET) != 0) {
        perror("Failed to seek to JPEG start position in CR3 file");
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }

    FILE *output_stream = NULL;
    if (to_stdout) {
        output_stream = stdout;
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        if (verbose)
            fprintf(stderr, "Largest JPEG preview found (size: %zu bytes), streaming to stdout...\n", jpeg_size);
    } else {
        output_stream = fopen(output_path, "wb");
        if (!output_stream) {
            perror("Failed to open output JPEG file");
            fclose(cr3_file);
            free(jpegs);
            return -1;
        }
        if (verbose)
            printf("Largest JPEG preview extracted to %s (size: %zu bytes)\n", output_path, jpeg_size);
    }

    size_t remaining = jpeg_size;
    unsigned char buffer[STREAM_BUFFER_SIZE];
    while (remaining > 0) {
        size_t to_read = remaining < STREAM_BUFFER_SIZE ? remaining : STREAM_BUFFER_SIZE;
        size_t bytes_read = fread(buffer, 1, to_read, cr3_file);
        if (bytes_read == 0) {
            if (ferror(cr3_file)) {
                perror("Error reading JPEG data from CR3 file");
            } else {
                fprintf(stderr, "Unexpected end-of-file while streaming JPEG data.\n");
            }
            fclose(cr3_file);
            if (!to_stdout && output_stream) fclose(output_stream);
            free(jpegs);
            return -1;
        }
        size_t bytes_written = fwrite(buffer, 1, bytes_read, output_stream);
        if (bytes_written != bytes_read) {
            if (to_stdout) {
                if (ferror(stdout)) perror("Error writing JPEG data to stdout");
                else fprintf(stderr, "Failed to write complete JPEG data to stdout (expected %zu, wrote %zu bytes).\n",
                             bytes_read, bytes_written);
            } else {
                fprintf(stderr, "Failed to write complete JPEG data to file %s (expected %zu, wrote %zu bytes).\n",
                        output_path, bytes_read, bytes_written);
            }
            fclose(cr3_file);
            if (!to_stdout && output_stream) fclose(output_stream);
            free(jpegs);
            return -1;
        }
        remaining -= bytes_read;
    }

    fclose(cr3_file);
    if (!to_stdout && output_stream) fclose(output_stream);
    free(jpegs);
    return 0;
}

// Updated extract_all_jpegs with size_t and our new heuristic
int extract_all_jpegs(const char *cr3_path, int verbose) {
    FILE *cr3_file = fopen(cr3_path, "rb");
    if (!cr3_file) {
        perror("Failed to open CR3 file");
        return -1;
    }
    fseek(cr3_file, 0, SEEK_END);
    size_t fileSize = ftell(cr3_file);
    rewind(cr3_file);
    JpegInfo *jpegs = NULL;
    int jpeg_count = 0;
    if (find_all_jpegs(cr3_file, &jpegs, &jpeg_count) != 0) {
        fprintf(stderr, "Failed to scan for JPEG previews in CR3 file.\n");
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }
    if (jpeg_count == 0) {
        fprintf(stderr, "No JPEG previews found in CR3 file: %s\n", cr3_path);
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }

    unsigned char *exifSegment = NULL;
    size_t exifSize = 0;
    if (!extractCr3Exif_streaming(cr3_file, fileSize, &exifSegment, &exifSize, verbose)) {
        if (verbose)
            fprintf(stderr, "Failed to extract EXIF from CR3 file (continuing without EXIF).\n");
    } else if (g_minimize_exif) {
        if (!minimizeExifData(&exifSegment, &exifSize)) {
            fprintf(stderr, "Failed to minimize EXIF data (continuing without EXIF).\n");
            free(exifSegment);
            exifSegment = NULL;
        }
    }

    // New heuristic: if the first JPEG segment is below 8KB and there are at least 4 segments,
    // skip the first segment by setting starting_index to 1.
    int starting_index = 0;
    if (jpeg_count >= 4 && jpegs[0].size < 8 * 1024) {
        if (verbose)
            fprintf(stderr, "First JPEG segment size %zu is below 8KB, skipping it.\n", jpegs[0].size);
        starting_index = 1;
    }
    int max_extract = ((jpeg_count - starting_index) < 3) ? (jpeg_count - starting_index) : 3;
    int result = 0;
    for (int i = starting_index; i < starting_index + max_extract; i++) {
        size_t start_offset = jpegs[i].start;
        size_t size_jpeg = jpegs[i].size;
        if (fseek(cr3_file, start_offset, SEEK_SET) != 0) {
            perror("Failed to seek to JPEG segment");
            result = -1;
            break;
        }
        unsigned char *jpeg_data = (unsigned char *)malloc(size_jpeg);
        if (!jpeg_data) {
            perror("Failed to allocate memory for JPEG data");
            result = -1;
            break;
        }
        if (fread(jpeg_data, 1, size_jpeg, cr3_file) != size_jpeg) {
            perror("Failed to read JPEG data");
            free(jpeg_data);
            result = -1;
            break;
        }
        unsigned char *output_data = jpeg_data;
        size_t output_size = size_jpeg;
        if (exifSegment) {
            if (!insertExifIntoJpeg(jpeg_data, size_jpeg, exifSegment, exifSize, &output_data, &output_size)) {
                fprintf(stderr, "Failed to insert EXIF into JPEG %d (using original JPEG).\n", i + 1);
                output_data = jpeg_data;
                output_size = size_jpeg;
            } else {
                free(jpeg_data);
            }
        }
        char *outfile = generate_output_filename_all((g_output_filename != NULL ? g_output_filename : cr3_path), i);
        if (!outfile) {
            fprintf(stderr, "Failed to generate output filename for JPEG %d\n", i + 1);
            free(output_data);
            result = -1;
            break;
        }
        FILE *outf = fopen(outfile, "wb");
        if (!outf) {
            perror("Failed to open output file");
            free(outfile);
            free(output_data);
            result = -1;
            break;
        }
        size_t written = fwrite(output_data, 1, output_size, outf);
        if (written != output_size) {
            fprintf(stderr, "Failed to write complete JPEG data to file %s (expected %zu, wrote %zu bytes).\n",
                    outfile, output_size, written);
            free(outfile);
            free(output_data);
            fclose(outf);
            result = -1;
            break;
        }
        if (verbose)
            fprintf(stderr, "Extracted JPEG %d to %s (size: %zu bytes) with %sEXIF\n",
                    i + 1, outfile, output_size, (exifSegment ? (g_minimize_exif ? "minimized " : "full ") : "no "));
        fclose(outf);
        free(outfile);
        free(output_data);
    }
    if (exifSegment) free(exifSegment);
    free(jpegs);
    fclose(cr3_file);
    return result;
}

// Updated extract_specific_jpeg with size_t and our new mapping heuristic.
// If the first segment is invalid (below 8KB) and there are at least 4 segments,
// we adjust the mapping so that -j 1 extracts jpegs[1], -j 2 extracts jpegs[2], and -j 3 extracts jpegs[3].
int extract_specific_jpeg(const char *cr3_path, int jpeg_index, int to_stdout, int verbose) {
    FILE *cr3_file = fopen(cr3_path, "rb");
    if (!cr3_file) {
        perror("Failed to open CR3 file");
        return -1;
    }
    fseek(cr3_file, 0, SEEK_END);
    size_t fileSize = ftell(cr3_file);
    rewind(cr3_file);
    JpegInfo *jpegs = NULL;
    int jpeg_count = 0;
    if (find_all_jpegs(cr3_file, &jpegs, &jpeg_count) != 0) {
        fprintf(stderr, "Failed to scan for JPEG previews in CR3 file.\n");
        fclose(cr3_file);
        return -1;
    }
    int idx = 0;
    // Adjust index mapping if the first JPEG segment is too small.
    if (jpeg_count >= 4 && jpegs[0].size < 8 * 1024) {
        if (jpeg_index < 1 || jpeg_index > (jpeg_count - 1)) {
            fprintf(stderr, "Requested JPEG index %d not available after skipping the invalid first segment. Only %d valid JPEG segments available.\n", jpeg_index, jpeg_count - 1);
            fclose(cr3_file);
            free(jpegs);
            return -1;
        }
        if (verbose)
            fprintf(stderr, "First JPEG segment size %zu is below 8KB, adjusting extraction index from %d to %d.\n", jpegs[0].size, jpeg_index, jpeg_index);
        idx = jpeg_index; // e.g., -j 1 now maps to array index 1.
    } else {
        if (jpeg_index < 1 || jpeg_index > jpeg_count) {
            fprintf(stderr, "Requested JPEG index %d not available. Only %d JPEG segments found.\n", jpeg_index, jpeg_count);
            fclose(cr3_file);
            free(jpegs);
            return -1;
        }
        idx = jpeg_index - 1;
    }
    if (fseek(cr3_file, jpegs[idx].start, SEEK_SET) != 0) {
        perror("Failed to seek to JPEG start position in CR3 file");
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }
    size_t jpeg_size = jpegs[idx].size;
    unsigned char *jpeg_data = (unsigned char *)malloc(jpeg_size);
    if (!jpeg_data) {
        perror("Failed to allocate memory for JPEG data");
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }
    if (fread(jpeg_data, 1, jpeg_size, cr3_file) != jpeg_size) {
        perror("Failed to read JPEG data from CR3 file");
        free(jpeg_data);
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }
    unsigned char *exifSegment = NULL;
    size_t exifSize = 0;
    if (!extractCr3Exif_streaming(cr3_file, fileSize, &exifSegment, &exifSize, verbose)) {
        if (verbose)
            fprintf(stderr, "Failed to extract EXIF from CR3 file (continuing without EXIF).\n");
    } else if (g_minimize_exif) {
        if (!minimizeExifData(&exifSegment, &exifSize)) {
            fprintf(stderr, "Failed to minimize EXIF data (continuing without EXIF).\n");
            free(exifSegment);
            exifSegment = NULL;
        }
    }
    unsigned char *output_data = jpeg_data;
    size_t output_size = jpeg_size;
    if (exifSegment) {
        if (!insertExifIntoJpeg(jpeg_data, jpeg_size, exifSegment, exifSize, &output_data, &output_size)) {
            fprintf(stderr, "Failed to insert EXIF into JPEG (using original JPEG).\n");
            output_data = jpeg_data;
            output_size = jpeg_size;
        } else {
            free(jpeg_data);
        }
        free(exifSegment);
    }
    FILE *outf = NULL;
    char *outfile = NULL;
    if (to_stdout) {
        outf = stdout;
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    } else {
        if (g_output_filename != NULL) {
            outfile = strdup(g_output_filename);
        } else {
            outfile = generate_output_filename_all(cr3_path, idx);
        }
        if (!outfile) {
            fprintf(stderr, "Failed to generate output filename for JPEG %d\n", jpeg_index);
            free(output_data);
            fclose(cr3_file);
            free(jpegs);
            return -1;
        }
        outf = fopen(outfile, "wb");
        if (!outf) {
            perror("Failed to open output file");
            free(outfile);
            free(output_data);
            fclose(cr3_file);
            free(jpegs);
            return -1;
        }
    }
    size_t written = fwrite(output_data, 1, output_size, outf);
    if (written != output_size) {
        fprintf(stderr, "Failed to write complete JPEG data.\n");
        if (!to_stdout) {
            free(outfile);
            fclose(outf);
        }
        free(output_data);
        fclose(cr3_file);
        free(jpegs);
        return -1;
    }
    if (verbose) {
        if (to_stdout)
            fprintf(stderr, "Extracted JPEG %d to stdout (size: %zu bytes) with %sEXIF\n",
                    jpeg_index, output_size, (exifSegment ? (g_minimize_exif ? "minimized " : "full ") : "no "));
        else
            fprintf(stderr, "Extracted JPEG %d to %s (size: %zu bytes) with %sEXIF\n",
                    jpeg_index, outfile, output_size, (exifSegment ? (g_minimize_exif ? "minimized " : "full ") : "no "));
    }
    if (!to_stdout) {
        fclose(outf);
        free(outfile);
    }
    free(output_data);
    fclose(cr3_file);
    free(jpegs);
    return 0;
}

// generate_output_filename (unchanged)
char* generate_output_filename(const char* source) {
    char *output = malloc(strlen(source) + 5);
    if (!output) {
        fprintf(stderr, "Failed to allocate memory for output filename\n");
        return NULL;
    }
    strcpy(output, source);
    char *dot = strrchr(output, '.');
    if (dot && dot != output && *(dot + 1) != '\0')
        *dot = '\0';
    strcat(output, ".jpg");
    return output;
}

// generate_output_filename_all (unchanged)
char* generate_output_filename_all(const char* source, int index) {
    char *base = malloc(strlen(source) + 1);
    if (!base) return NULL;
    strcpy(base, source);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
    }
    int outsize = strlen(base) + 1 + 3 + 4 + 1;
    char *outfile = malloc(outsize);
    if (!outfile) {
        free(base);
        return NULL;
    }
    snprintf(outfile, outsize, "%s_%03d.jpg", base, index + 1);
    free(base);
    return outfile;
}

// main (unchanged)
int main(int argc, char *argv[]) {
    int to_stdout = 0, verbose = 0;
    const char *cr3_path = NULL;
    char *output_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-") == 0) {
            to_stdout = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-m") == 0) {
            g_minimize_exif = 1;
        } else if (strcmp(argv[i], "-j") == 0) {
            if (i + 1 < argc) {
                if (strcmp(argv[i + 1], "all") == 0) {
                    g_extract_all = 1;
                    i++;
                } else if (strcmp(argv[i + 1], "1") == 0 ||
                           strcmp(argv[i + 1], "2") == 0 ||
                           strcmp(argv[i + 1], "3") == 0) {
                    g_extract_index = atoi(argv[i + 1]);
                    i++;
                } else {
                    fprintf(stderr, "Expected 'all', '1', '2' or '3' after '-j'\n");
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                fprintf(stderr, "Expected parameter after '-j'\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                g_output_filename = argv[i + 1];
                i++;
            } else {
                fprintf(stderr, "Expected filename after '-o'\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (!cr3_path) {
            cr3_path = argv[i];
        } else {
            fprintf(stderr, "Multiple input files specified.\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!cr3_path) {
        fprintf(stderr, "No input CR3 file specified.\n");
        print_usage(argv[0]);
        return 1;
    }

    if (g_extract_all) {
        if (to_stdout) {
            fprintf(stderr, "Cannot use stdout output with '-j all' option.\n");
            return 1;
        }
        int result = extract_all_jpegs(cr3_path, verbose);
        if (result == 0 && verbose) {
            fprintf(stderr, "Extraction of first 3 JPEGs completed successfully.\n");
        } else if (result != 0) {
            fprintf(stderr, "Extraction failed.\n");
        }
        return (result == 0) ? 0 : 1;
    } else if (g_extract_index != -1) {
        int result = extract_specific_jpeg(cr3_path, g_extract_index, to_stdout, verbose);
        if (result == 0 && verbose) {
            fprintf(stderr, "Extraction of JPEG %d completed successfully.\n", g_extract_index);
        } else if (result != 0) {
            fprintf(stderr, "Extraction failed.\n");
        }
        return (result == 0) ? 0 : 1;
    } else {
        if (!to_stdout) {
            if (g_output_filename != NULL) {
                output_path = strdup(g_output_filename);
            } else {
                output_path = generate_output_filename(cr3_path);
            }
            if (!output_path)
                return 1;
        }
        int result = extract_largest_jpeg(cr3_path, output_path, to_stdout, verbose);
        if (result == 0 && verbose) {
            fprintf(stderr, "Extraction completed successfully.\n");
        } else if (result != 0) {
            fprintf(stderr, "Extraction failed.\n");
        }
        if (output_path) free(output_path);
        return (result == 0) ? 0 : 1;
    }
}

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

static int g_verbose = 0;  // Global verbose flag

// ----- File I/O -----

// Reads entire file into memory. Returns pointer and sets *size.
unsigned char *readFile(const char *filename, size_t *size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open file %s\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buffer = (unsigned char *)malloc(*size);
    if (!buffer) {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(f);
        return NULL;
    }
    if (fread(buffer, 1, *size, f) != *size) {
        fprintf(stderr, "Failed to read file %s\n", filename);
        free(buffer);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buffer;
}

// Writes data to file. Returns 1 on success.
int writeFile(const char *filename, const unsigned char *data, size_t size) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Cannot write to file %s\n", filename);
        return 0;
    }
    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Failed to write file %s\n", filename);
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

// ----- Endian Helpers -----

// Read 16-bit little-endian value from data at offset.
uint16_t read16le(const unsigned char *data, size_t offset, size_t dataSize) {
    if (offset + 1 >= dataSize) return 0;
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

// Read 32-bit little-endian value from data at offset.
uint32_t read32le(const unsigned char *data, size_t offset, size_t dataSize) {
    if (offset + 3 >= dataSize) return 0;
    return ((uint32_t)data[offset]) | ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}

// ----- Streaming Box Parser for CR3 Source -----
// This function scans the file (using fseek/fread) to locate a box with the given 4-char type.
// It returns 1 if found (allocating *result with the box’s content, excluding the header),
// otherwise returns 0.
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
        uint32_t size32 = (header[0] << 24) | (header[1] << 16) |
                          (header[2] << 8) | header[3];
        char boxType[5];
        memcpy(boxType, header + 4, 4);
        boxType[4] = '\0';
        uint64_t boxSize = size32;
        size_t headerSize = 8;
        if (size32 == 1) {  // extended size
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

// ----- EXIF Extraction for CR3 using Streaming -----
// Opens the source CR3 file and streams it to locate the "moov" box and then the "uuid" box.
// Then it searches within the uuid box for the TIFF header ("II" then marker 42) and
// returns an EXIF segment (with "Exif\0\0" prepended).
int extractCr3Exif_streaming(const char *srcFilename, unsigned char **exifSegment, size_t *exifSize) {
    FILE *f = fopen(srcFilename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open source file %s\n", srcFilename);
        return 0;
    }
    // Determine file size.
    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Find "moov" box.
    unsigned char *moovBox = NULL;
    size_t moovSize = 0;
    if (!findBox_streaming(f, 0, fileSize, "moov", &moovBox, &moovSize)) {
         if (g_verbose) fprintf(stderr, "No 'moov' box found in CR3 file.\n");
         fclose(f);
         return 0;
    }
    // Within the moov box (now in memory), search for "uuid" box.
    // For simplicity, we use a memory-based scan here.
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
                 fclose(f);
                 return 0;
             }
             memcpy(uuidBox, moovBox + pos + headerSize, uuidSize);
             break;
         }
         pos += boxSize;
    }
    free(moovBox);
    if (!uuidBox) {
         if (g_verbose) fprintf(stderr, "No 'uuid' box found in 'moov' box.\n");
         fclose(f);
         return 0;
    }
    // In uuidBox, search for TIFF header ("II" followed by marker 42).
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
         if (g_verbose) fprintf(stderr, "No valid TIFF header found in 'uuid' box.\n");
         free(uuidBox);
         fclose(f);
         return 0;
    }
    size_t tiffDataSize = uuidSize - pos;
    unsigned char *tiffData = (unsigned char *)malloc(tiffDataSize);
    if (!tiffData) {
         fprintf(stderr, "Memory allocation failed for TIFF data\n");
         free(uuidBox);
         fclose(f);
         return 0;
    }
    memcpy(tiffData, uuidBox + pos, tiffDataSize);
    free(uuidBox);
    // Prepend standard EXIF header "Exif\0\0".
    const char exifHeader[6] = {'E','x','i','f',0,0};
    *exifSize = 6 + tiffDataSize;
    *exifSegment = (unsigned char *)malloc(*exifSize);
    if (!*exifSegment) {
         fprintf(stderr, "Memory allocation failed for EXIF segment\n");
         free(tiffData);
         fclose(f);
         return 0;
    }
    memcpy(*exifSegment, exifHeader, 6);
    memcpy(*exifSegment + 6, tiffData, tiffDataSize);
    free(tiffData);
    fclose(f);
    return 1;
}

// ----- Ensure EXIF Header -----
// Makes sure the EXIF segment begins with "Exif\0\0".
void ensureExifHeader(unsigned char **exifSegment, size_t *exifSize) {
    const char header[6] = {'E','x','i','f',0,0};
    if (*exifSize < 6 || memcmp(*exifSegment, header, 6) != 0) {
        size_t newSize = 6 + *exifSize;
        unsigned char *newSegment = (unsigned char *)malloc(newSize);
        if(newSegment) {
            memcpy(newSegment, header, 6);
            memcpy(newSegment+6, *exifSegment, *exifSize);
            free(*exifSegment);
            *exifSegment = newSegment;
            *exifSize = newSize;
        }
    }
}

// ----- Minimize EXIF Data -----
// Keeps only a minimal whitelist of essential IFD0 tags.
// Allowed tags: 0x010F (Make), 0x0110 (Model), 0x0132 (DateTime),
// 0x829A (ExposureTime), 0x829D (FNumber), 0x8827 (ISO Speed),
// 0x920A (FocalLength), 0x0112 (Orientation).
int minimizeExifData(unsigned char **exifSegment, size_t *exifSize) {
    const char *exifHeader = "Exif\0\0";
    if (*exifSize < 10 || memcmp(*exifSegment, exifHeader, 6) != 0) {
        fprintf(stderr, "Not a valid EXIF segment.\n");
        return 0;
    }
    size_t tiffStart = 6;
    if (tiffStart + 8 > *exifSize)
        return 0;
    uint32_t ifd0RelOffset = read32le(*exifSegment, tiffStart + 4, *exifSize);
    size_t ifd0Offset = tiffStart + ifd0RelOffset;
    if (ifd0Offset + 2 > *exifSize)
        return 0;
    uint16_t count = read16le(*exifSegment, ifd0Offset, *exifSize);
    size_t ifd0EntriesStart = ifd0Offset + 2;
    size_t ifd0EntriesSize = count * 12;
    if (ifd0EntriesStart + ifd0EntriesSize + 4 > *exifSize)
        return 0;
    
    uint16_t allowed[] = { 0x010F, 0x0110, 0x0132, 0x829A, 0x829D, 0x8827, 0x920A, 0x0112 };
    size_t allowedCount = sizeof(allowed) / sizeof(allowed[0]);
    
    unsigned char *filteredEntries = (unsigned char *)malloc(ifd0EntriesSize);
    if (!filteredEntries) {
        fprintf(stderr, "Memory allocation failed in minimizeExifData\n");
        return 0;
    }
    size_t newEntryCount = 0;
    size_t i;
    for (i = 0; i < count; i++) {
         size_t entryOffset = ifd0EntriesStart + i * 12;
         if (entryOffset + 12 > *exifSize)
              break;
         uint16_t tag = read16le(*exifSegment, entryOffset, *exifSize);
         int keep = 0;
         size_t j;
         for (j = 0; j < allowedCount; j++) {
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
         fprintf(stderr, "Memory allocation failed for new EXIF segment.\n");
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

// ----- Insert EXIF Segment into JPEG -----
// Inserts the provided EXIF segment (with proper header) immediately after the SOI marker.
int insertExifIntoJpeg(const unsigned char *jpegData, size_t jpegSize,
                         unsigned char *exifSegment, size_t exifSize,
                         unsigned char **outputData, size_t *outputSize) {
    if (jpegSize < 2 || jpegData[0] != 0xFF || jpegData[1] != 0xD8) {
         fprintf(stderr, "Destination file is not a valid JPEG.\n");
         return 0;
    }
    
    ensureExifHeader(&exifSegment, &exifSize);
    
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

// ----- Main Application -----
int main(int argc, char **argv) {
    // Usage: exifcopy_noexiv2 <source_cr3> <destination_jpeg> [-v]
    if (argc < 3 || argc > 4) {
         fprintf(stderr, "Usage: %s <source_cr3> <destination_jpeg> [-v]\n", argv[0]);
         return 1;
    }
    const char *srcPath = argv[1];
    const char *dstPath = argv[2];
    if (argc == 4 && strcmp(argv[3], "-v") == 0) {
         g_verbose = 1;
    }
    
    unsigned char *exifSegment = NULL;
    size_t exifSize = 0;
    
    // Use streaming method for CR3 source.
    if (!extractCr3Exif_streaming(srcPath, &exifSegment, &exifSize)) {
         fprintf(stderr, "Failed to extract EXIF from CR3 source file.\n");
         return 1;
    }
    
    if (!minimizeExifData(&exifSegment, &exifSize)) {
         fprintf(stderr, "Error minimizing EXIF data.\n");
         free(exifSegment);
         return 1;
    }
    
    size_t dstSize = 0;
    unsigned char *dstData = readFile(dstPath, &dstSize);
    if (!dstData) {
         free(exifSegment);
         return 1;
    }
    
    unsigned char *outputData = NULL;
    size_t outputSize = 0;
    if (!insertExifIntoJpeg(dstData, dstSize, exifSegment, exifSize, &outputData, &outputSize)) {
         fprintf(stderr, "Failed to insert EXIF into destination JPEG.\n");
         free(dstData);
         free(exifSegment);
         return 1;
    }
    free(dstData);
    
    if (!writeFile(dstPath, outputData, outputSize)) {
         fprintf(stderr, "Failed to write modified JPEG to %s\n", dstPath);
         free(outputData);
         free(exifSegment);
         return 1;
    }
    if (g_verbose) {
         printf("Successfully copied and minimized EXIF from %s to %s\n", srcPath, dstPath);
    }
    free(outputData);
    free(exifSegment);
    return 0;
}

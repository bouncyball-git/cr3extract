#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

typedef struct {
    long start;
    long end;
    long size;
} JpegInfo;

#define STREAM_BUFFER_SIZE 4096

// Function to find all JPEG segments in the file (modified to handle boundary conditions)
int find_all_jpegs(FILE *file, JpegInfo **jpegs, int *count) {
    const size_t BUFFER_SIZE = 4096; // Buffer size for reading file chunks
    unsigned char buffer[BUFFER_SIZE];
    size_t bytes_read;
    long file_pos = 0;
    int capacity = 10;
    *count = 0;

    *jpegs = malloc(capacity * sizeof(JpegInfo));
    if (!*jpegs) {
        perror("Failed to allocate memory for JPEG array");
        return -1;
    }

    long start = -1;
    unsigned char last_byte = 0;
    int has_last_byte = 0;

    rewind(file);

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        // Check for markers spanning across buffer boundaries
        if (has_last_byte && bytes_read > 0) {
            if (last_byte == 0xFF && buffer[0] == 0xD8) {
                start = file_pos - 1;
            }
            if (start != -1 && last_byte == 0xFF && buffer[0] == 0xD9) {
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
                start = -1;
            }
        }

        // Check for markers within the current buffer
        for (size_t i = 0; i < bytes_read - 1; i++) {
            if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) {
                start = file_pos + i;
            }
            if (start != -1 && buffer[i] == 0xFF && buffer[i + 1] == 0xD9) {
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
                start = -1;
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

// Modified extract function with streaming reading and stdout option, verbose control, and improved error messages
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
        if (jpegs[i].size > jpegs[largest_idx].size) {
            largest_idx = i;
        }
    }

    long jpeg_start_offset = jpegs[largest_idx].start;
    long jpeg_size = jpegs[largest_idx].size;

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
            fprintf(stderr, "Largest JPEG preview found (size: %ld bytes), streaming to stdout...\n", jpeg_size);
    } else {
        output_stream = fopen(output_path, "wb");
        if (!output_stream) {
            perror("Failed to open output JPEG file");
            fclose(cr3_file);
            free(jpegs);
            return -1;
        }
        if (verbose)
            printf("Largest JPEG preview extracted to %s (size: %ld bytes)\n", output_path, jpeg_size);
    }

    // Stream the JPEG data directly from source to destination
    long remaining = jpeg_size;
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
            if (!to_stdout && output_stream)
                fclose(output_stream);
            free(jpegs);
            return -1;
        }
        size_t bytes_written = fwrite(buffer, 1, bytes_read, output_stream);
        if (bytes_written != bytes_read) {
            if (to_stdout) {
                if (ferror(stdout)) {
                    perror("Error writing JPEG data to stdout");
                } else {
                    fprintf(stderr, "Failed to write complete JPEG data to stdout (expected %zu, wrote %zu bytes).\n", bytes_read, bytes_written);
                }
            } else {
                fprintf(stderr, "Failed to write complete JPEG data to file %s (expected %zu, wrote %zu bytes).\n", output_path, bytes_read, bytes_written);
            }
            fclose(cr3_file);
            if (!to_stdout && output_stream)
                fclose(output_stream);
            free(jpegs);
            return -1;
        }
        remaining -= bytes_read;
    }

    fclose(cr3_file);
    if (!to_stdout && output_stream)
        fclose(output_stream);
    free(jpegs);
    return 0;
}

// Function to generate output filename from source
char* generate_output_filename(const char* source) {
    char *output = malloc(strlen(source) + 5); // .jpg + null terminator
    if (!output) {
        perror("Failed to allocate memory for output filename");
        return NULL;
    }
    strcpy(output, source);
    char *dot = strrchr(output, '.');
    if (dot) {
        if (dot != output && *(dot + 1) != '\0') {
            *dot = '\0';
        }
    }
    strcat(output, ".jpg");
    return output;
}

int main(int argc, char *argv[]) {
    int to_stdout = 0, verbose = 0;
    const char *cr3_path = NULL;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            to_stdout = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            if (!cr3_path) {
                cr3_path = argv[i];
            } else {
                fprintf(stderr, "Multiple input files specified.\n");
                fprintf(stderr, "Usage: %s <source.CR3> [-] [-v]\n", argv[0]);
                return 1;
            }
        }
    }

    if (!cr3_path) {
        fprintf(stderr, "No input CR3 file specified.\n");
        fprintf(stderr, "Usage: %s <source.CR3> [-] [-v]\n", argv[0]);
        return 1;
    }

    char *output_path = NULL;
    if (!to_stdout) {
        output_path = generate_output_filename(cr3_path);
        if (!output_path) {
            return 1;
        }
    }

    int result = extract_largest_jpeg(cr3_path, output_path, to_stdout, verbose);

    if (result == 0) {
        if (verbose) {
            if (!to_stdout) {
                fprintf(stderr, "Extraction completed successfully.\n");
            } else {
                fprintf(stderr, "Extraction to stdout completed.\n");
            }
        }
    } else {
        fprintf(stderr, "Extraction failed.\n");
    }

    if (output_path) {
        free(output_path);
    }

    return (result == 0) ? 0 : 1;
}

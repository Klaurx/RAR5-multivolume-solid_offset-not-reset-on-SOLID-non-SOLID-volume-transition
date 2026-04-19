#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TARGET_FILE  "file_b.bin"
#define BLOCK_SIZE   (64 * 1024)

/* ---------------------------------------------------------- */
/* Hex output                                                 */
/* ---------------------------------------------------------- */
static void bytes_to_hex(const uint8_t *b, size_t len, char *out) {
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = h[(b[i] >> 4) & 0xF];
        out[i*2+1] = h[b[i] & 0xF];
    }
    out[len*2] = '\0';
}

/* ---------------------------------------------------------- */
/* Main                                                       */
/* ---------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <part1.rar> <part2.rar> <expected_sha256>\n",
            argv[0]);
        return 1;
    }

    const char *part1_path    = argv[1];
    const char *part2_path    = argv[2];
    const char *expected_hex  = argv[3];

    if (strlen(expected_hex) != 64) {
        fprintf(stderr, "[-] expected_sha256 must be 64 hex chars\n");
        return 1;
    }

    /* ---- Build NULL-terminated filename array for multivolume ---- */
    /* archive_read_open_filenames() is the correct API for feeding   */
    /* multiple volumes to libarchive's RAR5 handler.                 */
    const char *filenames[3];
    filenames[0] = part1_path;
    filenames[1] = part2_path;
    filenames[2] = NULL;          /* sentinel */

    /* ---- Create archive reader ---- */
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_rar5(a);

    /* ---- Open both volumes at once ---- */
    int r = archive_read_open_filenames(a, filenames, BLOCK_SIZE);
    if (r != ARCHIVE_OK) {
        fprintf(stderr, "[-] Failed to open archives: %s\n",
                archive_error_string(a));
        archive_read_free(a);
        return 1;
    }
    printf("[*] Opened: %s + %s\n", part1_path, part2_path);

    /* ---- Walk entries ---- */
    struct archive_entry *entry;
    int found = 0;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);

    uint8_t *buf = malloc(BLOCK_SIZE);
    size_t total = 0;
    uint8_t digest[32];
    char actual_hex[65];

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        printf("[*] Entry: %-30s  %lld bytes\n",
               name, (long long)archive_entry_size(entry));

        if (strcmp(name, TARGET_FILE) != 0) {
            archive_read_data_skip(a);
            continue;
        }

        found = 1;
        printf("[*] Extracting '%s'...\n", name);

        ssize_t n;
        while ((n = archive_read_data(a, buf, BLOCK_SIZE)) > 0) {
            EVP_DigestUpdate(mdctx, buf, (size_t)n);
            total += (size_t)n;
        }

        if (n < 0) {
            fprintf(stderr, "[-] Read error: %s\n",
                    archive_error_string(a));
        }
    }

    if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
        fprintf(stderr, "[!] archive_read_next_header: %s\n",
                archive_error_string(a));
    }

    archive_read_close(a);
    archive_read_free(a);

    EVP_DigestFinal_ex(mdctx, digest, NULL);
    EVP_MD_CTX_free(mdctx);
    free(buf);

    if (!found) {
        fprintf(stderr, "[-] '%s' not found in archive.\n", TARGET_FILE);
        return 1;
    }

    bytes_to_hex(digest, 32, actual_hex);

    /* ---- Report ---- */
    printf("\n===== result =====\n");
    printf("Bytes extracted:    %zu\n",  total);
    printf("libarchive SHA-256: %s\n",   actual_hex);
    printf("expected   SHA-256: %s\n",   expected_hex);

    if (strcmp(actual_hex, expected_hex) == 0) {
        printf("\n[PASS] Hashes match — bug not triggered. As expected\n");
        return 0;
    } else {
        printf("\n[FAIL] Hash mismatch — solid_offset corruption possible.\n");
        printf("       This is the PoC demonstrating the bug.\n");
        return 2;
    }
}

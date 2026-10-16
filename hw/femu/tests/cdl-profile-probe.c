/* Use the shipped host client's parser against captured controller responses. */
#ifndef CDL_HOST_PARSER_SOURCE
#error "Build with CDL_HOST_PARSER_SOURCE naming tools/cdl-ctrl.c"
#endif
#define main cdl_control_main
#include CDL_HOST_PARSER_SOURCE
#undef main

static int read_image(const char *path, uint8_t *image, size_t length)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    int ok = fread(image, 1, length, file) == length && fgetc(file) == EOF;
    fclose(file);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    uint8_t id[4096];
    uint8_t *config = NULL, *stats = NULL;
    uint16_t classes;
    uint32_t total;
    char *end;
    unsigned long vid, did;
    if (argc != 6) {
        return 2;
    }
    vid = strtoul(argv[4], &end, 0);
    if (*end || vid > UINT32_MAX) {
        return 2;
    }
    did = strtoul(argv[5], &end, 0);
    if (*end || did > UINT32_MAX) {
        return 2;
    }
    quiet_errors = true;
    device_path = "qtest-profile";
    if (read_image(argv[1], id, sizeof(id))) {
        return 2;
    }
    if (validate_identity(vid, did, id, &classes)) {
        return 1;
    }
    size_t config_len = 64 + (size_t)classes * 16;
    size_t stats_len = 64 + (size_t)classes * 24;
    config = malloc(config_len);
    stats = malloc(stats_len);
    if (!config || !stats || read_image(argv[2], config, config_len) ||
        read_image(argv[3], stats, stats_len)) {
        free(config);
        free(stats);
        return 2;
    }
    int invalid = validate_header(config, CDL_CONFIG_LID, classes, 16, &total) ||
                  validate_header(stats, CDL_STATS_LID, classes, 24, &total) ||
                  memcmp(config + 32, stats + 32, 16);
    free(config);
    free(stats);
    return invalid ? 1 : 0;
}

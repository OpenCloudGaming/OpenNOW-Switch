#include <switch.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define OPENNOW_DEFAULT_NRO "sdmc:/switch/SwitchNOW/SwitchNOW.nro"

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F')
        return 10 + ch - 'A';
    return -1;
}

static bool decode_value(const char* source, char* destination, size_t size)
{
    size_t written = 0;
    while (*source)
    {
        if (written + 1 >= size)
            return false;
        if (*source != '%')
        {
            destination[written++] = *source++;
            continue;
        }
        if (!source[1] || !source[2])
            return false;
        const int high = hex_value(source[1]);
        const int low = hex_value(source[2]);
        if (high < 0 || low < 0)
            return false;
        destination[written++] = (char)((high << 4) | low);
        source += 3;
    }
    destination[written] = '\0';
    return true;
}

static bool make_manifest_path(const char* nro_path, char* output, size_t size)
{
    if (!nro_path || !*nro_path || strlen(nro_path) + 1 > size)
        return false;
    strcpy(output, nro_path);
    char* extension = strrchr(output, '.');
    if (!extension || strcasecmp(extension, ".nro"))
        return false;
    if ((size_t)(extension - output) + strlen(".opennow") + 1 > size)
        return false;
    strcpy(extension, ".opennow");
    return true;
}

static void read_target_path(
    const char* manifest_path, char* output, size_t output_size)
{
    snprintf(output, output_size, "%s", OPENNOW_DEFAULT_NRO);
    FILE* file = fopen(manifest_path, "rb");
    if (!file)
        return;

    char line[4096];
    while (fgets(line, sizeof(line), file))
    {
        static const char prefix[] = "nro_path=";
        if (strncmp(line, prefix, sizeof(prefix) - 1))
            continue;
        char* end = strpbrk(line, "\r\n");
        if (end)
            *end = '\0';
        char decoded[FS_MAX_PATH];
        if (decode_value(line + sizeof(prefix) - 1, decoded, sizeof(decoded)) &&
            !strncmp(decoded, "sdmc:/", 6))
            snprintf(output, output_size, "%s", decoded);
        break;
    }
    fclose(file);
}

int main(int argc, char* argv[])
{
    if (argc < 1 || !argv[0])
        return 1;

    char manifest_path[FS_MAX_PATH];
    if (!make_manifest_path(argv[0], manifest_path, sizeof(manifest_path)))
        return 1;

    FILE* manifest = fopen(manifest_path, "rb");
    if (!manifest)
        return 1;
    fclose(manifest);

    char target_path[FS_MAX_PATH];
    read_target_path(manifest_path, target_path, sizeof(target_path));

    char next_arguments[2048];
    const int length = snprintf(
        next_arguments, sizeof(next_arguments), "\"%s\" \"%s\"",
        target_path, manifest_path);
    if (length < 0 || (size_t)length >= sizeof(next_arguments))
        return 1;

    envSetNextLoad(target_path, next_arguments);
    return 0;
}

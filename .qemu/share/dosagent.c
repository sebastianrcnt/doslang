#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <conio.h>
#include <bios.h>

#define LINE_SIZE 9000
#define CHUNK_SIZE 4096

static char linebuf[LINE_SIZE];
static unsigned char data[CHUNK_SIZE + 1];

#define COM1_BASE 0x3F8

static void serial_init(void) {
    outp(COM1_BASE + 1, 0x00);
    outp(COM1_BASE + 3, 0x80);
    outp(COM1_BASE + 0, 12);
    outp(COM1_BASE + 1, 0);
    outp(COM1_BASE + 3, 0x03);
    outp(COM1_BASE + 2, 0xC7);
    outp(COM1_BASE + 4, 0x0B);
}

static int console_break_requested(void) {
    unsigned short key = _bios_keybrd(_KEYBRD_READY);
    if ((key & 0xFF) != 3) return 0;
    _bios_keybrd(_KEYBRD_READ);
    return 1;
}

static int serial_getc(void) {
    while ((inp(COM1_BASE + 5) & 0x01) == 0) {
        if (console_break_requested()) return -1;
    }
    return inp(COM1_BASE);
}

static void serial_putc(int c) {
    while ((inp(COM1_BASE + 5) & 0x20) == 0) { }
    outp(COM1_BASE, c);
}

static void serial_write(const char *text) {
    while (*text) serial_putc((unsigned char)*text++);
}

static int serial_readline(char *buffer, int cap) {
    int used = 0;
    int c;
    for (;;) {
        c = serial_getc();
        if (c < 0) return -1;
        if (c == '\r' || c == '\n') {
            if (used == 0) continue;
            buffer[used] = '\0';
            return used;
        }
        if (used < cap - 1) buffer[used++] = (char)c;
    }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int decode_hex(const char *src, unsigned char *dst, int cap) {
    int n = 0;
    int hi, lo;
    while (*src && src[1]) {
        if (n >= cap) return -1;
        hi = hexval((unsigned char)src[0]);
        lo = hexval((unsigned char)src[1]);
        if (hi < 0 || lo < 0) return -1;
        dst[n++] = (unsigned char)((hi << 4) | lo);
        src += 2;
    }
    return *src ? -1 : n;
}

static void print_hex(const unsigned char *src, unsigned count) {
    static const char digits[] = "0123456789ABCDEF";
    unsigned i;
    for (i = 0; i < count; ++i) {
        serial_putc(digits[src[i] >> 4]);
        serial_putc(digits[src[i] & 15]);
    }
}

static void ok_data(const unsigned char *src, unsigned count) {
    serial_write("OK ");
    print_hex(src, count);
    serial_write("\r\n");
}

static void error_text(const char *message) {
    serial_write("ERR ");
    print_hex((const unsigned char *)message, strlen(message));
    serial_write("\r\n");
}

static int decode_path(const char *hex, char *path, int cap) {
    int n = decode_hex(hex, (unsigned char *)path, cap - 1);
    if (n < 0) return 0;
    path[n] = '\0';
    return 1;
}

static void command_read(char *args) {
    char path[260];
    char *offset_text = strchr(args, ' ');
    FILE *file;
    long offset;
    size_t count;
    int eof;

    if (!offset_text) { error_text("READ requires path and offset"); return; }
    *offset_text++ = '\0';
    if (!decode_path(args, path, sizeof(path))) { error_text("Invalid path encoding"); return; }
    offset = atol(offset_text);
    file = fopen(path, "rb");
    if (!file) { error_text("Cannot open file"); return; }
    if (fseek(file, offset, SEEK_SET) != 0) { fclose(file); error_text("Cannot seek file"); return; }
    count = fread(data, 1, CHUNK_SIZE, file);
    eof = feof(file) ? 1 : 0;
    if (count < CHUNK_SIZE) eof = 1;
    fclose(file);
    sprintf(linebuf, "OK %d ", eof);
    serial_write(linebuf);
    print_hex(data, count);
    serial_write("\r\n");
}

static void command_write(char *args) {
    char path[260];
    char *mode_text = strchr(args, ' ');
    char *payload;
    FILE *file;
    int count;

    if (!mode_text) { error_text("WRITE requires path, mode and data"); return; }
    *mode_text++ = '\0';
    payload = strchr(mode_text, ' ');
    if (!payload) { error_text("WRITE requires data"); return; }
    *payload++ = '\0';
    if (!decode_path(args, path, sizeof(path))) { error_text("Invalid path encoding"); return; }
    count = decode_hex(payload, data, CHUNK_SIZE);
    if (count < 0) { error_text("Invalid data encoding"); return; }
    file = fopen(path, mode_text[0] == 'A' ? "ab" : "wb");
    if (!file) { error_text("Cannot write file"); return; }
    if (count && fwrite(data, 1, count, file) != (size_t)count) {
        fclose(file); error_text("Short write"); return;
    }
    fclose(file);
    ok_data((const unsigned char *)"", 0);
}

static void command_list(char *args) {
    char path[260];
    char pattern[300];
    char output[CHUNK_SIZE];
    struct find_t found;
    unsigned used = 0;
    int rc;

    if (!decode_path(args, path, sizeof(path))) { error_text("Invalid path encoding"); return; }
    strcpy(pattern, path);
    if (pattern[0] && pattern[strlen(pattern) - 1] != '\\' && pattern[strlen(pattern) - 1] != '/') strcat(pattern, "\\");
    strcat(pattern, "*.*");
    rc = _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_SUBDIR | _A_ARCH, &found);
    while (rc == 0) {
        char entry[100];
        int len;
        if (strcmp(found.name, ".") && strcmp(found.name, "..")) {
            sprintf(entry, "%s\t%lu\t%s\n", found.name, found.size, (found.attrib & _A_SUBDIR) ? "DIR" : "FILE");
            len = strlen(entry);
            if (used + len >= sizeof(output)) break;
            memcpy(output + used, entry, len);
            used += len;
        }
        rc = _dos_findnext(&found);
    }
    ok_data((const unsigned char *)output, used);
}

static void command_exec(char *args) {
    char command[700];
    char shell_command[800];
    const char *temp_path = "C:\\PIEXEC.TMP";
    FILE *file;
    int n, exit_code;
    size_t count;

    n = decode_hex(args, (unsigned char *)command, sizeof(command) - 1);
    if (n < 0) { error_text("Invalid command encoding"); return; }
    command[n] = '\0';
    sprintf(shell_command, "COMMAND.COM /C %s > %s", command, temp_path);
    exit_code = system(shell_command);
    file = fopen(temp_path, "rb");
    count = file ? fread(data, 1, CHUNK_SIZE, file) : 0;
    if (file) fclose(file);
    remove(temp_path);
    sprintf(linebuf, "OK %d ", exit_code);
    serial_write(linebuf);
    print_hex(data, count);
    serial_write("\r\n");
}

int main(void) {
    char *command;
    char *args;

    serial_init();
    cprintf("\r\n[DOSAGENT] Ready on COM1. Press Ctrl+C here to stop.\r\n");
    serial_write("DOSAGENT READY\r\n");
    for (;;) {
        if (serial_readline(linebuf, sizeof(linebuf)) < 0) {
            cprintf("\r\n[DOSAGENT] Stopped from VGA console.\r\n");
            break;
        }
        command = linebuf;
        args = strchr(command, ' ');
        if (args) *args++ = '\0'; else args = command + strlen(command);

        cprintf("[DOSAGENT] %s\r\n", command);

        if (!strcmp(command, "PING")) ok_data((const unsigned char *)"PONG", 4);
        else if (!strcmp(command, "READ")) command_read(args);
        else if (!strcmp(command, "WRITE")) command_write(args);
        else if (!strcmp(command, "LIST")) command_list(args);
        else if (!strcmp(command, "EXEC")) command_exec(args);
        else if (!strcmp(command, "QUIT")) { ok_data((const unsigned char *)"BYE", 3); break; }
        else error_text("Unknown command");
    }
    return 0;
}

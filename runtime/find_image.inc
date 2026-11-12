#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#endif

static char exe_dir[512] = ".";

static void init_exe_dir(void) {
#ifdef _WIN32
    char p[512];
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    if (n > 0 && n < sizeof p) {
        char *s = strrchr(p, '\\');
        if (s) { *s = 0; snprintf(exe_dir, sizeof exe_dir, "%s", p); }
    }
#endif
}

/* saves (NVRAM files), the log and debug dumps live in <exe dir>/saves */
static void setup_data_dir(void) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s/saves", exe_dir);
#ifdef _WIN32
    _mkdir(g_data_dir);
#endif
}

/* Resolve a .cue to the .bin it references. */
static void cue_to_bin(const char *cue, char *out, size_t n) {
    snprintf(out, n, "%s", cue);
    FILE *f = fopen(cue, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!strncmp(line, "FILE", 4) && q1 && q2) {
            *q2 = 0;
            char dir[512];
            snprintf(dir, sizeof dir, "%s", cue);
            char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
            if (bs > sl) sl = bs;
            if (sl) { sl[1] = 0; snprintf(out, n, "%s%s", dir, q1 + 1); }
            else snprintf(out, n, "%s", q1 + 1);
            break;
        }
    }
    if (f) fclose(f);
}

/* Is this a raw 2352-byte-sector CD-i image? (disc label "CD-I " at sector 16) */
static int is_cdi_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8] = {0};
    int ok = fseek(f, 16L * 2352 + 24 + 1, SEEK_SET) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "CD-I ", 5);
    fclose(f);
    return ok;
}

static int scan_dir_for_image(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char p[800];
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (is_cdi_image(p)) { FindClose(h); snprintf(out, n, "%s", p); return 0; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)dir; (void)out; (void)n;
#endif
    return -1;
}

/* a .cue, a .bin or a folder holding the image */
static int resolve_image(const char *path, char *out, size_t n) {
    size_t L = strlen(path);
#ifdef _WIN32
    DWORD at = GetFileAttributesA(path);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
        char romdir[700];
        if (!scan_dir_for_image(path, out, n)) return 0;
        snprintf(romdir, sizeof romdir, "%s\\rom", path);
        return scan_dir_for_image(romdir, out, n);
    }
#endif
    if (L > 4 && !_stricmp(path + L - 4, ".cue")) cue_to_bin(path, out, n);
    else snprintf(out, n, "%s", path);
    return is_cdi_image(out) ? 0 : -1;
}

/* The disc location chosen in the installer or the first-run dialog is kept
   in <exe dir>/saves/disc_path.txt (portable: nothing outside the folder). */
static void disc_path_file(char *p, size_t n) { snprintf(p, n, "%s/disc_path.txt", g_data_dir); }

static int saved_image(char *out, size_t n) {
    char p[700], line[1024];
    disc_path_file(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int r = -1;
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) r = resolve_image(line, out, n);
    }
    fclose(f);
    return r;
}

static void save_image_path(const char *path) {
    char p[700];
    disc_path_file(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", path); fclose(f); }
}

#ifdef _WIN32
#include <commdlg.h>
/* first run without a disc: ask for the .cue/.bin (returns 0 when one was picked) */
static int pick_image_dialog(char *out, size_t n) {
    for (;;) {
        char file[1024] = "";
        OPENFILENAMEA o;
        memset(&o, 0, sizeof o);
        o.lStructSize = sizeof o;
        o.lpstrFilter = "CD-i disc image (*.cue, *.bin)\0*.cue;*.bin\0All files\0*.*\0";
        o.lpstrFile = file;
        o.nMaxFile = sizeof file;
        o.lpstrTitle = "Select your " GAME_TITLE " disc image (.cue or .bin)";
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&o)) return -1;
        if (!resolve_image(file, out, n)) { save_image_path(file); return 0; }
        MessageBoxA(NULL, "That file is not a CD-i disc image. Please select the game's .cue or .bin file.",
                    GAME_TITLE, MB_ICONWARNING);
    }
}
#endif

static int find_image(const char *arg, char *out, size_t n) {
    if (arg) return resolve_image(arg, out, n);
    if (!saved_image(out, n)) return 0;
    /* look for rom/*.bin next to the working directory or the executable, up to 4 levels up */
    const char *bases[2] = {".", exe_dir};
    for (int b = 0; b < 2; b++) {
        char dir[600];
        snprintf(dir, sizeof dir, "%s", bases[b]);
        for (int up = 0; up <= 4; up++) {
            char romdir[700];
            snprintf(romdir, sizeof romdir, "%s\\rom", dir);
            if (!scan_dir_for_image(romdir, out, n)) return 0;
            if (!scan_dir_for_image(dir, out, n)) return 0;
            size_t L = strlen(dir);
            if (L + 3 < sizeof dir) strcat(dir, "\\..");
        }
    }
    return -1;
}


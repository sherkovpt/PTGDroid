/* EFSRV: RFs / RFile / CFileMan / CDir mapped onto host directories.
 * E: = the memory card (game files supplied by the user), C: = a writable host directory.
 * Symbian paths are case-insensitive; resolution matches that on case-sensitive hosts. */
#include "hle.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_host(p) _mkdir(p)
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#define mkdir_host(p) mkdir(p, 0755)
#endif

extern char g_card_root[512];
char g_c_root[512] = "userdata_c";

/* ---- path translation ---- */
static int wild_match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (const char *t = s;; t++) {
                if (wild_match(pat + 1, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (!*s || (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s))) return 0;
    }
    return !*s;
}

#ifndef _WIN32
/* Replace each component with the existing host entry that matches case-insensitively. */
static void fix_case(char *path) {
    char *p = path + (path[0] == '/');
    char built[1024];
    snprintf(built, sizeof built, "%s", path[0] == '/' ? "/" : ".");
    while (*p) {
        char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char comp[256];
        snprintf(comp, sizeof comp, "%.*s", (int)n, p);
        DIR *d = opendir(built);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strlen(e->d_name) == n && !strncasecmp(e->d_name, comp, n)) { memcpy(p, e->d_name, n); break; }
            }
            closedir(d);
        }
        size_t bl = strlen(built);
        snprintf(built + bl, sizeof built - bl, "%s%.*s", bl && built[bl - 1] != '/' ? "/" : "", (int)n, p);
        if (!slash) break;
        p = slash + 1;
    }
}
#endif

/* Symbian path descriptor -> host path. Returns 0 if the drive is unknown. */
static int host_path(uint32_t des, char *out, size_t n) {
    char sym[600];
    des_to_cstr(des, 1, sym, sizeof sym);
    const char *rest = sym;
    const char *root = g_c_root;
    if (sym[0] && sym[1] == ':') {
        char drive = (char)tolower((unsigned char)sym[0]);
        if (drive == 'e' || drive == 'd') root = g_card_root;
        else if (drive != 'c' && drive != 'z') return 0;
        rest = sym + 2;
    }
    snprintf(out, n, "%s/%s", root, rest[0] == '\\' ? rest + 1 : rest);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
#ifndef _WIN32
    fix_case(out);
#endif
    return 1;
}

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && (st.st_mode & S_IFDIR); }
static int exists(const char *p) { struct stat st; return !stat(p, &st); }

static int32_t missing_error(const char *path) {
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *s = strrchr(parent, '/');
    if (s) *s = 0;
    return is_dir(parent) ? KErrNotFound : KErrPathNotFound;
}

/* ---- handles ---- */
typedef struct { FILE *f; int write; } File;

static void RFs_Connect(CPU *c) { wr32(ARG(c, 0), handle_new(H_OTHER, NULL)); RET(KErrNone); }
static void RFsBase_Close(CPU *c) {
    uint32_t h = rd32(ARG(c, 0));
    File *f = handle_type(h) == H_OTHER ? handle_obj(h) : NULL;
    if (f) { fclose(f->f); free(f); }
    handle_close(h);
    wr32(ARG(c, 0), 0);
}

static File *file_of(uint32_t rfile) { return handle_obj(rd32(rfile)); }

static void file_open(CPU *c, int create) {
    char path[1024];
    uint32_t rfile = ARG(c, 0), mode = ARG(c, 3);
    int write = (mode & 0x200) != 0 || create;
    if (!host_path(ARG(c, 2), path, sizeof path)) { RET(KErrNotReady); return; }
    LOG("fs: %s %s (mode %x)%s", create ? "create" : "open", path, mode, exists(path) ? "" : " [missing]");
    if (create && exists(path)) { RET(KErrAlreadyExists); return; }
    if (!create && !exists(path)) { RET(missing_error(path)); return; }
    FILE *f = fopen(path, create ? "w+b" : (write ? "r+b" : "rb"));
    if (!f) { RET(create ? missing_error(path) : KErrAccessDenied); return; }
    File *fo = calloc(1, sizeof *fo);
    fo->f = f;
    fo->write = write;
    wr32(rfile, handle_new(H_OTHER, fo));
    RET(KErrNone);
}
static void RFile_Open(CPU *c) { file_open(c, 0); }
static void RFile_Create(CPU *c) { file_open(c, 1); }

static void file_read(CPU *c, uint32_t des, uint32_t len) {
    File *f = file_of(ARG(c, 0));
    if (!f) { RET(KErrArgument); return; }
    uint32_t max = des_max(des);
    if (len > max) len = max;
    size_t got = fread(MEM + des_ptr(des), 1, len, f->f);
    des_setlen(des, (uint32_t)got);
    RET(KErrNone);
}
static void RFile_Read(CPU *c) { uint32_t d = ARG(c, 1); file_read(c, d, des_max(d)); }
static void RFile_ReadLen(CPU *c) { file_read(c, ARG(c, 1), ARG(c, 2)); }

static void RFile_Write(CPU *c) {
    File *f = file_of(ARG(c, 0));
    uint32_t d = ARG(c, 1);
    if (!f || !f->write) { RET(KErrAccessDenied); return; }
    size_t n = fwrite(MEM + des_ptr(d), 1, des_len(d), f->f);
    RET(n == des_len(d) ? KErrNone : -26 /* KErrDiskFull */);
}

/* TInt RFile::Seek(TSeek aMode, TInt& aPos) const */
static void RFile_Seek(CPU *c) {
    File *f = file_of(ARG(c, 0));
    uint32_t mode = ARG(c, 1), posp = ARG(c, 2);
    if (!f) { RET(KErrArgument); return; }
    int32_t pos = (int32_t)rd32(posp);
    int whence = mode == 2 ? SEEK_CUR : (mode == 3 ? SEEK_END : SEEK_SET);
    fseek(f->f, pos, whence);
    wr32(posp, (uint32_t)ftell(f->f));
    RET(KErrNone);
}
static void RFile_Size(CPU *c) {
    File *f = file_of(ARG(c, 0));
    if (!f) { RET(KErrArgument); return; }
    long cur = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    wr32(ARG(c, 1), (uint32_t)ftell(f->f));
    fseek(f->f, cur, SEEK_SET);
    RET(KErrNone);
}
static void RFile_SetSize(CPU *c) {
    File *f = file_of(ARG(c, 0));
    if (!f) { RET(KErrArgument); return; }
    fflush(f->f);
#ifdef _WIN32
    int r = _chsize(_fileno(f->f), (long)ARG(c, 1));
#else
    int r = ftruncate(fileno(f->f), (off_t)ARG(c, 1));
#endif
    RET(r ? KErrAccessDenied : KErrNone);
}

/* ---- RFs operations ---- */
static void RFs_Delete(CPU *c) {
    char path[1024];
    if (!host_path(ARG(c, 1), path, sizeof path)) { RET(KErrNotReady); return; }
    LOG("fs: delete %s", path);
    RET(remove(path) ? missing_error(path) : KErrNone);
}

static void RFs_MkDirAll(CPU *c) {
    char path[1024];
    if (!host_path(ARG(c, 1), path, sizeof path)) { RET(KErrNotReady); return; }
    char *last = strrchr(path, '/');
    if (last) *last = 0; /* MkDirAll takes "dir\\" or "dir\\file": create the directory part */
    if (is_dir(path)) { RET(KErrAlreadyExists); return; }
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir_host(path); *p = '/'; }
    }
    RET(mkdir_host(path) && !is_dir(path) ? KErrPathNotFound : KErrNone);
}

/* TEntry: iAtt@0, iSize@4, iModified@8, iType@16 (3 uids), iName@28 (TBufC<256>) */
#define TENTRY_SIZE 544
#define KEntryAttDir 0x10
static void fill_entry(uint32_t e, const char *host, const char *name) {
    struct stat st;
    memset(MEM + e, 0, TENTRY_SIZE);
    if (!stat(host, &st)) {
        wr32(e, (st.st_mode & S_IFDIR) ? KEntryAttDir : 0);
        wr32(e + 4, (uint32_t)st.st_size);
    }
    uint32_t n = (uint32_t)strlen(name);
    if (n > 256) n = 256;
    wr32(e + 28, n);
    for (uint32_t i = 0; i < n; i++) wr16(e + 32 + 2 * i, (uint8_t)name[i]);
}

static void RFs_Entry(CPU *c) {
    char path[1024];
    if (!host_path(ARG(c, 1), path, sizeof path)) { RET(KErrNotReady); return; }
    if (!exists(path)) { RET(missing_error(path)); return; }
    const char *name = strrchr(path, '/');
    fill_entry(ARG(c, 2), path, name ? name + 1 : path);
    RET(KErrNone);
}

static void TEntry_ctor(CPU *c) { memset(MEM + ARG(c, 0), 0, TENTRY_SIZE); }
static void TEntry_copy(CPU *c) { memmove(MEM + ARG(c, 0), MEM + ARG(c, 1), TENTRY_SIZE); }
static void TEntry_IsDir(CPU *c) { RET((rd32(ARG(c, 0)) & KEntryAttDir) != 0); }

/* CDir: host object; entries array pointer @4, count @8 */
static void cdir_dtor(CPU *c) {
    uint32_t t = ARG(c, 0);
    gfree(rd32(t + 4));
    if (ARG(c, 1) & 1) gfree(t);
}
static void CDir_Count(CPU *c) { RET(rd32(ARG(c, 0) + 8)); }
static void CDir_At(CPU *c) { RET(rd32(ARG(c, 0) + 4) + ARG(c, 1) * TENTRY_SIZE); }

/* TInt RFs::GetDir(const TDesC& aName, TUint aAttMask, TUint aSortKey, CDir*& aList) const */
static void RFs_GetDir(CPU *c) {
    char path[1024];
    uint32_t attmask = ARG(c, 2), outp = ARG(c, 4);
    wr32(outp, 0);
    if (!host_path(ARG(c, 1), path, sizeof path)) { RET(KErrNotReady); return; }
    char *slash = strrchr(path, '/');
    const char *pattern = slash && slash[1] ? slash + 1 : "*";
    char dir[1024];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 0, path);
    if (!is_dir(dir)) { RET(KErrPathNotFound); return; }

    char names[256][260];
    int n = 0;
#ifdef _WIN32
    char q[1100];
    snprintf(q, sizeof q, "%s/*", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(q, &fd);
    if (h != -1) {
        do {
            if (strcmp(fd.name, ".") && strcmp(fd.name, "..") && n < 256) snprintf(names[n++], 260, "%s", fd.name);
        } while (!_findnext(h, &fd));
        _findclose(h);
    }
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && n < 256) snprintf(names[n++], 260, "%s", e->d_name);
    }
    if (d) closedir(d);
#endif
    GuestFn v[] = { cdir_dtor };
    uint32_t list = host_object("CDir", 16, v, 1);
    uint32_t arr = gallocz(TENTRY_SIZE * (uint32_t)(n ? n : 1));
    uint32_t count = 0;
    for (int i = 0; i < n; i++) {
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        if (!wild_match(pattern, names[i])) continue;
        if (is_dir(full) && !(attmask & KEntryAttDir)) continue;
        fill_entry(arr + count * TENTRY_SIZE, full, names[i]);
        count++;
    }
    wr32(list + 4, arr);
    wr32(list + 8, count);
    wr32(outp, list);
    RET(KErrNone);
}

/* TVolumeInfo: iDrive(16) iUniqueID@16 iSize@20 iFree@28 iName@36 */
static void TVolumeInfo_ctor(CPU *c) { memset(MEM + ARG(c, 0), 0, 36 + 4 + 512); }
static void RFs_Volume(CPU *c) {
    uint32_t v = ARG(c, 1);
    wr32(v, 3); /* EMediaFlash */
    wr32(v + 20, 64u << 20); wr32(v + 24, 0);
    wr32(v + 28, 32u << 20); wr32(v + 32, 0);
    RET(KErrNone);
}

/* ---- CFileMan ---- */
static void CFileMan_dtor(CPU *c) { if (ARG(c, 1) & 1) gfree(ARG(c, 0)); }
static void CFileMan_NewL(CPU *c) {
    GuestFn v[] = { CFileMan_dtor };
    RET(host_object("CFileMan", 32, v, 1));
}
static void CFileMan_Delete(CPU *c) {
    char path[1024];
    if (!host_path(ARG(c, 1), path, sizeof path)) { RET(KErrNotReady); return; }
    LOG("fs: filemanager delete %s", path);
    RET(remove(path) ? missing_error(path) : KErrNone);
}
static void CFileMan_Copy(CPU *c) {
    char src[1024], dst[1024];
    if (!host_path(ARG(c, 1), src, sizeof src) || !host_path(ARG(c, 2), dst, sizeof dst)) { RET(KErrNotReady); return; }
    FILE *in = fopen(src, "rb");
    if (!in) { RET(missing_error(src)); return; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); RET(missing_error(dst)); return; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    RET(KErrNone);
}
static void CFileMan_RmDir(CPU *c) {
    char path[1024];
    if (!host_path(ARG(c, 1), path, sizeof path)) { RET(KErrNotReady); return; }
    LOG("CFileMan::RmDir(%s) not performed", path);
    RET(KErrNone);
}

const HleEntry HLE_FS[] = {
    {"efsrv", "Connect__3RFsi", RFs_Connect},
    {"efsrv", "Close__7RFsBase", RFsBase_Close},
    {"efsrv", "Open__5RFileR3RFsRC7TDesC16Ui", RFile_Open},
    {"efsrv", "Create__5RFileR3RFsRC7TDesC16Ui", RFile_Create},
    {"efsrv", "Read__C5RFileR5TDes8", RFile_Read},
    {"efsrv", "Read__C5RFileR5TDes8i", RFile_ReadLen},
    {"efsrv", "Write__5RFileRC6TDesC8", RFile_Write},
    {"efsrv", "Seek__C5RFile5TSeekRi", RFile_Seek},
    {"efsrv", "Size__C5RFileRi", RFile_Size},
    {"efsrv", "SetSize__5RFilei", RFile_SetSize},
    {"efsrv", "Delete__3RFsRC7TDesC16", RFs_Delete},
    {"efsrv", "MkDirAll__3RFsRC7TDesC16", RFs_MkDirAll},
    {"efsrv", "Entry__C3RFsRC7TDesC16R6TEntry", RFs_Entry},
    {"efsrv", "GetDir__C3RFsRC7TDesC16UiUiRP4CDir", RFs_GetDir},
    {"efsrv", "Volume__C3RFsR11TVolumeInfoi", RFs_Volume},
    {"efsrv", "__11TVolumeInfo", TVolumeInfo_ctor},
    {"efsrv", "__6TEntry", TEntry_ctor},
    {"efsrv", "__6TEntryRC6TEntry", TEntry_copy},
    {"efsrv", "IsDir__C6TEntry", TEntry_IsDir},
    {"efsrv", "Count__C4CDir", CDir_Count},
    {"efsrv", "__vc__C4CDiri", CDir_At},
    {"efsrv", "NewL__8CFileManR3RFs", CFileMan_NewL},
    {"efsrv", "Delete__8CFileManRC7TDesC16Ui", CFileMan_Delete},
    {"efsrv", "Copy__8CFileManRC7TDesC16T1Ui", CFileMan_Copy},
    {"efsrv", "RmDir__8CFileManRC7TDesC16", CFileMan_RmDir},
    {0, 0, 0},
};

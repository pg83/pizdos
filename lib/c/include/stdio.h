#pragma once

#include <__base.h>

__BEGIN_DECLS

typedef struct _IO_FILE FILE;

typedef struct {
    long long __pos;
    long long __pad;
} fpos_t;

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 4096
#define FOPEN_MAX 1000
#define TMP_MAX 10000
#define L_tmpnam 20
#define L_ctermid 20
#define P_tmpdir "/tmp"

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

extern FILE* const stdin;
extern FILE* const stdout;
extern FILE* const stderr;

#define stdin (stdin)
#define stdout (stdout)
#define stderr (stderr)

FILE* fopen(const char* __restrict, const char* __restrict);
FILE* freopen(const char* __restrict, const char* __restrict, FILE* __restrict);
FILE* fdopen(int, const char*);
FILE* fmemopen(void* __restrict, size_t, const char* __restrict);
FILE* open_memstream(char**, size_t*);
int fclose(FILE*);
int fflush(FILE*);
int fileno(FILE*);
int feof(FILE*);
int ferror(FILE*);
void clearerr(FILE*);

int fgetc(FILE*);
int getc(FILE*);
int getchar(void);
int ungetc(int, FILE*);
int fputc(int, FILE*);
int putc(int, FILE*);
int putchar(int);
char* fgets(char* __restrict, int, FILE* __restrict);
int fputs(const char* __restrict, FILE* __restrict);
int puts(const char*);
size_t fread(void* __restrict, size_t, size_t, FILE* __restrict);
size_t fwrite(const void* __restrict, size_t, size_t, FILE* __restrict);
ssize_t getline(char** __restrict, size_t* __restrict, FILE* __restrict);
ssize_t getdelim(char** __restrict, size_t* __restrict, int, FILE* __restrict);

int fseek(FILE*, long, int);
int fseeko(FILE*, off_t, int);
long ftell(FILE*);
off_t ftello(FILE*);
void rewind(FILE*);
int fgetpos(FILE* __restrict, fpos_t* __restrict);
int fsetpos(FILE*, const fpos_t*);

void setbuf(FILE* __restrict, char* __restrict);
int setvbuf(FILE* __restrict, char* __restrict, int, size_t);
void setbuffer(FILE*, char*, size_t);
void setlinebuf(FILE*);

int printf(const char* __restrict, ...) __PRINTF(1, 2);
int fprintf(FILE* __restrict, const char* __restrict, ...) __PRINTF(2, 3);
int sprintf(char* __restrict, const char* __restrict, ...) __PRINTF(2, 3);
int snprintf(char* __restrict, size_t, const char* __restrict, ...) __PRINTF(3, 4);
int dprintf(int, const char* __restrict, ...) __PRINTF(2, 3);
int asprintf(char**, const char*, ...) __PRINTF(2, 3);
int vprintf(const char* __restrict, va_list) __PRINTF(1, 0);
int vfprintf(FILE* __restrict, const char* __restrict, va_list) __PRINTF(2, 0);
int vsprintf(char* __restrict, const char* __restrict, va_list) __PRINTF(2, 0);
int vsnprintf(char* __restrict, size_t, const char* __restrict, va_list) __PRINTF(3, 0);
int vdprintf(int, const char* __restrict, va_list) __PRINTF(2, 0);
int vasprintf(char**, const char*, va_list) __PRINTF(2, 0);

int scanf(const char* __restrict, ...) __SCANF(1, 2);
int fscanf(FILE* __restrict, const char* __restrict, ...) __SCANF(2, 3);
int sscanf(const char* __restrict, const char* __restrict, ...) __SCANF(2, 3);
int vscanf(const char* __restrict, va_list) __SCANF(1, 0);
int vfscanf(FILE* __restrict, const char* __restrict, va_list) __SCANF(2, 0);
int vsscanf(const char* __restrict, const char* __restrict, va_list) __SCANF(2, 0);

void perror(const char*);

void flockfile(FILE*);
int ftrylockfile(FILE*);
void funlockfile(FILE*);
int getc_unlocked(FILE*);
int getchar_unlocked(void);
int putc_unlocked(int, FILE*);
int putchar_unlocked(int);
int fgetc_unlocked(FILE*);
int fputc_unlocked(int, FILE*);
int fflush_unlocked(FILE*);
size_t fread_unlocked(void*, size_t, size_t, FILE*);
size_t fwrite_unlocked(const void*, size_t, size_t, FILE*);
void clearerr_unlocked(FILE*);
int feof_unlocked(FILE*);
int ferror_unlocked(FILE*);
int fileno_unlocked(FILE*);
char* fgets_unlocked(char*, int, FILE*);
int fputs_unlocked(const char*, FILE*);

typedef ssize_t(cookie_read_function_t)(void*, char*, size_t);
typedef ssize_t(cookie_write_function_t)(void*, const char*, size_t);
typedef int(cookie_seek_function_t)(void*, off_t*, int);
typedef int(cookie_close_function_t)(void*);

typedef struct {
    cookie_read_function_t* read;
    cookie_write_function_t* write;
    cookie_seek_function_t* seek;
    cookie_close_function_t* close;
} cookie_io_functions_t;

FILE* fopencookie(void*, const char*, cookie_io_functions_t);

FILE* tmpfile(void);
char* tmpnam(char*);
char* tempnam(const char*, const char*);
FILE* popen(const char*, const char*);
int pclose(FILE*);

int remove(const char*);
int rename(const char*, const char*);
int renameat(int, const char*, int, const char*);
char* ctermid(char*);

__END_DECLS

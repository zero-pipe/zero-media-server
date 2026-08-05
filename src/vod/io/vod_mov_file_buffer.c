#include "mov_file_buffer.h"

#if defined(_WIN32) || defined(_WIN64)
#define zms_fseek64 _fseeki64
#define zms_ftell64 _ftelli64
#elif defined(__ANDROID__)
#define zms_fseek64 fseek
#define zms_ftell64 ftell
#elif defined(__linux__)
#define zms_fseek64 fseeko
#define zms_ftell64 ftello
#else
#define zms_fseek64 fseek
#define zms_ftell64 ftell
#endif

static int mov_file_read(void *fp, void *data, uint64_t bytes)
{
    if (bytes == fread(data, 1, bytes, (FILE *)fp)) {
        return 0;
    }
    return ferror((FILE *)fp) ? ferror((FILE *)fp) : -1;
}

static int mov_file_write(void *fp, const void *data, uint64_t bytes)
{
    return bytes == fwrite(data, 1, bytes, (FILE *)fp) ? 0 : ferror((FILE *)fp);
}

static int mov_file_seek(void *fp, int64_t offset)
{
    return zms_fseek64((FILE *)fp, offset, offset >= 0 ? SEEK_SET : SEEK_END);
}

static int64_t mov_file_tell(void *fp)
{
    return zms_ftell64((FILE *)fp);
}

const struct mov_buffer_t *zms_mov_file_buffer(void)
{
    static const struct mov_buffer_t s_io = {
        mov_file_read,
        mov_file_write,
        mov_file_seek,
        mov_file_tell,
    };
    return &s_io;
}

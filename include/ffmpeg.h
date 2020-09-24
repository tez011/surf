#pragma once
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include "libavutil/audio_fifo.h"
#include <libavutil/dict.h>
#include <libswresample/swresample.h>
}

#ifdef __cplusplus
#undef av_err2str
static inline char *av_err2str(int en)
{
	static char s[AV_ERROR_MAX_STRING_SIZE];
	memset(s, 0, AV_ERROR_MAX_STRING_SIZE);
	return av_make_error_string(s, AV_ERROR_MAX_STRING_SIZE, en);
}
#endif // __cplusplus

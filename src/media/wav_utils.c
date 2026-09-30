/*
 * wav_utils.c
 *
 *  Created on: 2021年10月11日
 *      Author: ZKSWE Develop Team
 */

#include <stdlib.h>
#include "wav_utils.h"
#include "audio_player.h"
#include "utils/Log.h"

#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

typedef struct {
	uint32_t riff_id;
	uint32_t riff_sz;
	uint32_t riff_fmt;
	uint32_t fmt_id;
	uint32_t fmt_sz;
	uint16_t audio_format;
	uint16_t num_channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	uint32_t data_id;
	uint32_t data_sz;
} wav_header_t;

typedef struct {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t wave_id;
} riff_wave_header_t;

typedef struct {
    uint32_t id;
    uint32_t sz;
} chunk_header_t;

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} chunk_fmt_t;

typedef struct {
	FILE *pf;
	wav_header_t header;
} wav_handle_t;

void* zk_wav_init(const char *path, uint16_t channels, uint32_t sample_rate) {
	wav_handle_t *handle = (wav_handle_t *) calloc(1, sizeof(wav_handle_t));

	handle->header.riff_id = ID_RIFF;
	handle->header.riff_sz = 0;
	handle->header.riff_fmt = ID_WAVE;
	handle->header.fmt_id = ID_FMT;
	handle->header.fmt_sz = 16;
	handle->header.audio_format = 1;
	handle->header.num_channels = channels;
	handle->header.sample_rate = sample_rate;
	handle->header.bits_per_sample = 16;
	handle->header.byte_rate = (handle->header.bits_per_sample / 8) * handle->header.num_channels * handle->header.sample_rate;
	handle->header.block_align = handle->header.num_channels * (handle->header.bits_per_sample / 8);
	handle->header.data_id = ID_DATA;

	handle->pf = fopen(path, "wb");
	if (!handle->pf) {
		LOGE("open path %s fail!\n", path);
		free(handle);
		return NULL;
	}

	fwrite(&handle->header, 1, sizeof(wav_header_t), handle->pf);

	return handle;
}

int zk_wav_add_data(void *handle, const uint8_t *data, uint32_t len) {
	if (!handle) {
		return -1;
	}

	wav_handle_t *wav_handle = (wav_handle_t *) handle;
	return fwrite(data, 1, len, (FILE *) wav_handle->pf);
}

int zk_wav_deinit(void *handle) {
	if (!handle) {
		return -1;
	}

	wav_handle_t *wav_handle = (wav_handle_t *) handle;
	uint32_t hlen = sizeof(wav_header_t);

	wav_handle->header.data_sz = (ftell(wav_handle->pf) - hlen) / wav_handle->header.block_align * wav_handle->header.block_align;

	fseek(wav_handle->pf, 0, SEEK_SET);
	fwrite(&wav_handle->header, 1, hlen, wav_handle->pf);
	fflush(wav_handle->pf);
	fclose(wav_handle->pf);

	free(wav_handle);

	return -1;
}

int zk_wav_play(const char *file) {
	riff_wave_header_t riff_wave_header;
	chunk_header_t chunk_header;
	chunk_fmt_t chunk_fmt;

	FILE *pf = fopen(file, "rb");
	if (!pf) {
		LOGE("Unable to open file '%s'\n", file);
		return -1;
	}

	fread(&riff_wave_header, sizeof(riff_wave_header), 1, pf);
	if ((riff_wave_header.riff_id != ID_RIFF) ||
		(riff_wave_header.wave_id != ID_WAVE)) {
		LOGE("Error: '%s' is not a riff/wave file\n", file);
		fclose(pf);
		return -1;
	}

	int more_chunks = 1;
	do {
		fread(&chunk_header, sizeof(chunk_header), 1, pf);

		switch (chunk_header.id) {
		case ID_FMT:
			fread(&chunk_fmt, sizeof(chunk_fmt), 1, pf);
			/* If the format header is larger, skip the rest */
			if (chunk_header.sz > sizeof(chunk_fmt)) {
				fseek(pf, chunk_header.sz - sizeof(chunk_fmt), SEEK_CUR);
			}
			break;
		case ID_DATA:
			/* Stop looking for chunks */
			more_chunks = 0;
			break;
		default:
			/* Unknown chunk, skip bytes */
			fseek(pf, chunk_header.sz, SEEK_CUR);
			break;
		}
	} while (more_chunks);

//	void *player = zk_audio_multi_player_init(chunk_fmt.num_channels, chunk_fmt.sample_rate, 0, 0);
	zk_audio_player_init(chunk_fmt.num_channels, chunk_fmt.sample_rate, 0, 0);
	uint32_t size = 4096;
	uint8_t data[size];
	int num_read;

	do {
		num_read = fread(data, 1, size, pf);
		if (num_read > 0) {
//			zk_audio_multi_player_put_frame(player, data, num_read);
			zk_audio_player_put_frame(data, num_read);
		}
	} while (num_read > 0);

//	zk_audio_multi_player_deinit(player);
	zk_audio_player_deinit();
	fclose(pf);

	return 0;
}

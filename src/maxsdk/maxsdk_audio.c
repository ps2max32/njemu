/* Audio on maxsdk's PCM stream (<mx/snd.h>).

   The stream is one output and mx_snd is not for two threads at once, so only
   the sound thread queues. The main thread reaches the stream only before that
   thread starts and after it ends; pausing just sets a flag. NCDZ's CDDA comes
   from a second thread: it writes to a ring that the sound thread mixes in, as
   the PS2 backend does on audsrv. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mx/snd.h>
#include <mx/thread.h>

#include "common/audio_driver.h"

#define MAX_VOLUME 100

/* a power of two: the ring is masked */
#define MP3_RING_SIZE (8192 * 2) /* samples, stereo interleaved */
#define MP3_RING_MASK (MP3_RING_SIZE - 1)
/* wake the writer when about one MP3 frame of room has appeared */
#define MP3_SIGNAL_THRESHOLD (736 * 2 * 2)

/* queueing gives up when the stream takes nothing for this long */
#define STALL_LIMIT_US 500000u
#define QUEUE_WAIT_US 2000u
#define QUEUE_CHUNK 8192u

typedef struct maxsdk_audio {
	uint16_t samples;
	bool is_mp3_channel;
} maxsdk_audio_t;

static volatile bool paused;

static int16_t *mp3_ring;
static volatile uint32_t mp3_write_pos;
static volatile uint32_t mp3_read_pos;
static volatile uint32_t mp3_last_read_pos;
static volatile uint32_t mp3_cancel_generation;
static volatile bool mp3_active;
static int mp3_sema = -1;

static void *maxsdk_init(void)
{
	return calloc(1, sizeof(maxsdk_audio_t));
}

static void maxsdk_free(void *data)
{
	maxsdk_audio_t *audio = (maxsdk_audio_t *)data;

	if (audio == NULL)
		return;
	if (audio->is_mp3_channel) {
		mp3_active = false;
		if (mp3_sema >= 0) {
			mx_sema_delete(mp3_sema);
			mp3_sema = -1;
		}
		free(mp3_ring);
		mp3_ring = NULL;
	}
	free(audio);
}

static int32_t maxsdk_volumeMax(void *data)
{
	(void)data;
	return MAX_VOLUME;
}

static bool maxsdk_chSRCReserve(void *data, uint16_t samples, int32_t frequency, uint8_t channels)
{
	maxsdk_audio_t *audio = (maxsdk_audio_t *)data;

	audio->samples = samples;
	audio->is_mp3_channel = false;
	paused = false;
	if (mx_snd_stream_format((uint32_t)frequency, 16, channels) < 0) {
		printf("audio: no stream of %d Hz, %d channels\n", (int)frequency, channels);
		return false;
	}
	mx_snd_stream_volume(MX_SND_VOL_MAX);
	return true;
}

static bool maxsdk_chReserve(void *data, uint16_t samplecount, uint8_t channels)
{
	maxsdk_audio_t *audio = (maxsdk_audio_t *)data;

	(void)channels;
	audio->is_mp3_channel = true;
	audio->samples = samplecount;
	if (mp3_ring == NULL) {
		mp3_ring = (int16_t *)calloc(MP3_RING_SIZE, sizeof(int16_t));
		if (mp3_ring == NULL)
			return false;
	}
	if (mp3_sema < 0)
		mp3_sema = mx_sema_create(1, 1);
	if (mp3_sema < 0) {
		free(mp3_ring);
		mp3_ring = NULL;
		return false;
	}
	mp3_write_pos = 0;
	mp3_read_pos = 0;
	mp3_last_read_pos = 0;
	mp3_cancel_generation = 0;
	mp3_active = true;
	return true;
}

static void maxsdk_release(void *data)
{
	maxsdk_audio_t *audio = (maxsdk_audio_t *)data;

	if (audio->is_mp3_channel) {
		mp3_active = false;
		if (mp3_sema >= 0)
			mx_sema_signal(mp3_sema); /* a writer waiting for room */
		if (mp3_ring != NULL)
			memset(mp3_ring, 0, MP3_RING_SIZE * sizeof(int16_t));
		mp3_write_pos = 0;
		mp3_read_pos = 0;
		mp3_last_read_pos = 0;
	} else {
		mx_snd_stream_stop();
	}
}

static void mix_mp3(int16_t *buffer, uint32_t sample_count)
{
	uint32_t read_pos = mp3_read_pos;
	uint32_t available;
	uint32_t i;

	if (!mp3_active || mp3_ring == NULL)
		return;
	available = (mp3_write_pos - read_pos) & MP3_RING_MASK;
	if (available > sample_count)
		available = sample_count;
	for (i = 0; i < available; i++) {
		int32_t mixed = (int32_t)buffer[i] + mp3_ring[read_pos];

		if (mixed > 32767)
			mixed = 32767;
		else if (mixed < -32768)
			mixed = -32768;
		buffer[i] = (int16_t)mixed;
		read_pos = (read_pos + 1) & MP3_RING_MASK;
	}
	mp3_read_pos = read_pos;
	if (((read_pos - mp3_last_read_pos) & MP3_RING_MASK) >= MP3_SIGNAL_THRESHOLD) {
		mp3_last_read_pos = read_pos;
		if (mp3_sema >= 0)
			mx_sema_signal(mp3_sema);
	}
}

static void queue_blocking(const uint8_t *data, uint32_t size)
{
	uint32_t waited = 0;

	while (size > 0) {
		uint32_t chunk = size > QUEUE_CHUNK ? QUEUE_CHUNK : size;
		uint32_t taken = mx_snd_stream_queue(data, chunk);

		data += taken;
		size -= taken;
		if (taken != 0) {
			waited = 0;
		} else if (waited >= STALL_LIMIT_US) {
			return; /* the stream is not playing: drop this block */
		}
		if (size > 0) {
			usleep(QUEUE_WAIT_US);
			waited += QUEUE_WAIT_US;
		}
	}
}

static void maxsdk_srcOutputBlocking(void *data, int32_t volume, void *buffer, uint32_t size)
{
	int16_t *samples = (int16_t *)buffer;
	uint32_t count = size / sizeof(int16_t);
	uint32_t i;

	(void)data;
	/* one stream, one volume: game sound is scaled here so that CDDA keeps
	   a volume of its own */
	if (paused) {
		memset(buffer, 0, size);
	} else if (volume < MAX_VOLUME) {
		if (volume < 0)
			volume = 0;
		for (i = 0; i < count; i++)
			samples[i] = (int16_t)(((int32_t)samples[i] * volume) / MAX_VOLUME);
	}
	if (!paused)
		mix_mp3(samples, count);
	queue_blocking((const uint8_t *)buffer, size);
}

static void maxsdk_outputPannedBlocking(void *data, int leftvol, int rightvol, void *buffer, uint32_t size)
{
	const int16_t *src = (const int16_t *)buffer;
	uint32_t count = size / sizeof(int16_t);
	uint32_t generation;
	uint32_t write_pos;
	uint32_t room;
	uint32_t i;

	(void)data;
	if (mp3_ring == NULL || !mp3_active)
		return;
	generation = mp3_cancel_generation;

	/* a block of zero volume silences CDDA at once: drop what is queued */
	if (leftvol <= 0 && rightvol <= 0) {
		mp3_cancel_generation++;
		mp3_read_pos = 0;
		mp3_write_pos = 0;
		mp3_last_read_pos = 0;
		memset(mp3_ring, 0, MP3_RING_SIZE * sizeof(int16_t));
		if (mp3_sema >= 0)
			mx_sema_signal(mp3_sema);
		return;
	}
	if ((count & 1) != 0 || count >= MP3_RING_SIZE)
		return;

	write_pos = mp3_write_pos;
	room = MP3_RING_SIZE - ((write_pos - mp3_read_pos) & MP3_RING_MASK) - 1;
	while (room < count) {
		if (mp3_sema >= 0)
			mx_sema_wait(mp3_sema);
		if (!mp3_active || generation != mp3_cancel_generation)
			return;
		room = MP3_RING_SIZE - ((write_pos - mp3_read_pos) & MP3_RING_MASK) - 1;
	}
	for (i = 0; i < count; i += 2) {
		mp3_ring[write_pos] = (int16_t)(((int32_t)src[i] * leftvol) / MAX_VOLUME);
		write_pos = (write_pos + 1) & MP3_RING_MASK;
		mp3_ring[write_pos] = (int16_t)(((int32_t)src[i + 1] * rightvol) / MAX_VOLUME);
		write_pos = (write_pos + 1) & MP3_RING_MASK;
	}
	mp3_write_pos = write_pos;
}

static void maxsdk_cancelPannedOutput(void *data)
{
	maxsdk_audio_t *audio = (maxsdk_audio_t *)data;

	if (audio == NULL || !audio->is_mp3_channel || !mp3_active)
		return;
	mp3_cancel_generation++;
	mp3_read_pos = mp3_write_pos;
	mp3_last_read_pos = mp3_read_pos;
	if (mp3_sema >= 0)
		mx_sema_signal(mp3_sema);
}

static void maxsdk_setPaused(void *data, bool pause)
{
	maxsdk_audio_t *audio = (maxsdk_audio_t *)data;

	/* The sound thread keeps feeding the stream, in silence, so that its
	   queue drains. This runs on the main thread: no mx_snd call here. */
	if (audio != NULL && !audio->is_mp3_channel)
		paused = pause;
}

audio_driver_t audio_maxsdk = {
	"maxsdk",
	maxsdk_init,
	maxsdk_free,
	maxsdk_volumeMax,
	maxsdk_chSRCReserve,
	maxsdk_chReserve,
	maxsdk_srcOutputBlocking,
	maxsdk_outputPannedBlocking,
	maxsdk_release,
	maxsdk_setPaused,
	maxsdk_cancelPannedOutput,
};

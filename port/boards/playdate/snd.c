/*
 * Sound for the Playdate.
 *
 * Quake's software mixer is not used. Each sound effect (an 8-bit WAV in the
 * pak) is decoded once to 16-bit mono PCM on first use and handed to one of a
 * few Playdate SamplePlayers, which the system mixes. Volume and left/right
 * balance are derived from the emitter's position relative to the listener.
 */

#include "quakedef.h"
#include "pd_port.h"

cvar_t bgmvolume = {"bgmvolume", "1", true};
cvar_t volume = {"volume", "0.7", true};
vec_t sound_nominal_clip_dist = 1000.0;
vec3_t listener_origin;
vec3_t listener_right;

#define MAX_SFX		512
#define NUM_PLAYERS	8
#define PCM_BUDGET	(1536 * 1024)	/* bytes of decoded samples to keep */

typedef struct {
	sfx_t sfx;		/* must be first: sfx_t* <-> snd_entry_t* */
	AudioSample *sample;
	int16_t *pcm;
	int bytes;
	int last_used;
} snd_entry_t;

typedef struct {
	SamplePlayer *player;
	snd_entry_t *entry;	/* non-NULL while (possibly) playing */
	AudioSample *bound;	/* what the player was last given */
	int entnum, entchannel;
	int started;
} snd_voice_t;

extern int len_for_emu;		/* set by COM_LoadFile */

static snd_entry_t entries[MAX_SFX];
static int num_entries;
static snd_voice_t voices[NUM_PLAYERS];
static int pcm_bytes;
static int tick;
static int snd_ready;


static uint32_t rd32(const byte *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t rd16(const byte *p)
{
	return p[0] | (p[1] << 8);
}

/* Decode a WAV file to 16-bit mono. Returns malloc'd PCM or NULL. */
static int16_t *decode_wav(const byte *wav, int size, uint32_t *rate, int *bytes)
{
	const byte *end = wav + size, *p = wav + 12;
	const byte *data = NULL;
	uint32_t datalen = 0;
	int channels = 1, bits = 8, have_fmt = 0;

	if (size < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4))
		return NULL;

	while (p + 8 <= end) {
		uint32_t len = rd32(p + 4);
		const byte *body = p + 8;

		if (!memcmp(p, "fmt ", 4) && body + 16 <= end) {
			channels = rd16(body + 2);
			*rate = rd32(body + 4);
			bits = rd16(body + 14);
			have_fmt = 1;
		} else if (!memcmp(p, "data", 4)) {
			data = body;
			datalen = len;
			if (body + datalen > end)
				datalen = (uint32_t) (end - body);
			break;
		}
		p = body + len + (len & 1);
	}
	if (!have_fmt || !data || channels < 1 || (bits != 8 && bits != 16))
		return NULL;

	int frames = datalen / (channels * (bits / 8));
	int16_t *pcm = malloc((size_t) frames * sizeof(int16_t) + 2);
	if (!pcm)
		return NULL;

	for (int i = 0; i < frames; i++) {
		if (bits == 8)	/* unsigned */
			pcm[i] = (int16_t) ((data[i * channels] - 128) << 8);
		else
			pcm[i] = (int16_t) rd16(data + i * channels * 2);
	}
	*bytes = frames * (int) sizeof(int16_t);
	return pcm;
}

static int in_use(const snd_entry_t *e)
{
	for (int i = 0; i < NUM_PLAYERS; i++)
		if (voices[i].entry == e)
			return 1;
	return 0;
}

static void unload(snd_entry_t *e)
{
	/* Players must not keep a pointer to a sample we are about to free */
	for (int i = 0; i < NUM_PLAYERS; i++) {
		if (voices[i].bound && voices[i].bound == e->sample) {
			qembd_pd->sound->sampleplayer->stop(voices[i].player);
			qembd_pd->sound->sampleplayer->setSample(voices[i].player, NULL);
			voices[i].bound = NULL;
		}
	}
	if (e->sample)
		qembd_pd->sound->sample->freeSample(e->sample);	/* frees pcm too */
	else
		free(e->pcm);
	pcm_bytes -= e->bytes;
	e->sample = NULL;
	e->pcm = NULL;
	e->bytes = 0;
}

/* Drop least-recently-used samples that no voice references */
static void trim_cache(void)
{
	while (pcm_bytes > PCM_BUDGET) {
		snd_entry_t *oldest = NULL;

		for (int i = 0; i < num_entries; i++) {
			snd_entry_t *e = &entries[i];
			if (e->sample && !in_use(e) &&
			    (!oldest || e->last_used < oldest->last_used))
				oldest = e;
		}
		if (!oldest)
			return;
		unload(oldest);
	}
}

static snd_entry_t *load(snd_entry_t *e)
{
	if (e->sample)
		return e;

	byte *wav = COM_LoadTempFile(e->sfx.name);
	if (!wav)
		return NULL;

	uint32_t rate = 11025;
	int bytes = 0;
	int16_t *pcm = decode_wav(wav, len_for_emu, &rate, &bytes);
	if (!pcm) {
		Con_DPrintf("S_LoadSound: unsupported wav %s\n", e->sfx.name);
		return NULL;
	}

	e->sample = qembd_pd->sound->sample->newSampleFromData(
		(uint8_t *) pcm, kSound16bitMono, rate, bytes, 1);
	if (!e->sample) {
		free(pcm);
		return NULL;
	}
	e->pcm = pcm;
	e->bytes = bytes;
	pcm_bytes += bytes;
	trim_cache();
	return e;
}

static snd_entry_t *find(const char *name)
{
	for (int i = 0; i < num_entries; i++)
		if (!strcmp(entries[i].sfx.name, name))
			return &entries[i];
	return NULL;
}

void S_Init(void)
{
	Con_Printf("Sound init\n");
	Cvar_RegisterVariable(&volume);
	Cvar_RegisterVariable(&bgmvolume);

	for (int i = 0; i < NUM_PLAYERS; i++) {
		voices[i].player = qembd_pd->sound->sampleplayer->newPlayer();
		if (!voices[i].player) {
			Con_Printf("Sound: could not allocate player %d\n", i);
			return;
		}
	}
	snd_ready = 1;
}

void S_Shutdown(void)
{
	Con_Printf("Sound shutdown\n");
	S_StopAllSounds(true);
	snd_ready = 0;
	for (int i = 0; i < NUM_PLAYERS; i++) {
		if (voices[i].player)
			qembd_pd->sound->sampleplayer->freePlayer(voices[i].player);
		voices[i].player = NULL;
	}
	for (int i = 0; i < num_entries; i++)
		if (entries[i].sample)
			unload(&entries[i]);
}

sfx_t *S_PrecacheSound(char *sample)
{
	char path[MAX_QPATH];
	snd_entry_t *e;

	if (!snd_ready)
		return NULL;

	snprintf(path, sizeof(path), "sound/%s", sample);
	e = find(path);
	if (!e) {
		if (num_entries >= MAX_SFX) {
			Con_Printf("S_PrecacheSound: too many sounds\n");
			return NULL;
		}
		e = &entries[num_entries++];
		memset(e, 0, sizeof(*e));
		strncpy(e->sfx.name, path, MAX_QPATH - 1);
	}
	return &e->sfx;
}

static snd_voice_t *pick_voice(int entnum, int entchannel)
{
	snd_voice_t *oldest = &voices[0];

	/* A given entity channel replaces its previous sound */
	if (entchannel != 0) {
		for (int i = 0; i < NUM_PLAYERS; i++)
			if (voices[i].entry && voices[i].entnum == entnum &&
			    voices[i].entchannel == entchannel)
				return &voices[i];
	}
	for (int i = 0; i < NUM_PLAYERS; i++) {
		if (!qembd_pd->sound->sampleplayer->isPlaying(voices[i].player))
			return &voices[i];
		if (voices[i].started < oldest->started)
			oldest = &voices[i];
	}
	return oldest;
}

void S_StartSound(int entnum, int entchannel, sfx_t *sfx, vec3_t origin,
                  float fvol, float attenuation)
{
	if (!snd_ready || !sfx)
		return;

	float vol = fvol * volume.value;
	float left = vol, right = vol;

	if (entnum != cl.viewentity && attenuation > 0) {
		vec3_t delta;
		VectorSubtract(origin, listener_origin, delta);
		float dist = Length(delta);
		float scale = 1.0f - dist * (attenuation / sound_nominal_clip_dist);

		if (scale <= 0)
			return;
		if (dist > 0) {
			float dot = DotProduct(delta, listener_right) / dist;
			left = vol * scale * (dot > 0 ? 1.0f - dot : 1.0f);
			right = vol * scale * (dot < 0 ? 1.0f + dot : 1.0f);
		}
	}
	if (left <= 0.01f && right <= 0.01f)
		return;

	snd_entry_t *e = load((snd_entry_t *) sfx);
	if (!e)
		return;
	e->last_used = ++tick;

	snd_voice_t *v = pick_voice(entnum, entchannel);
	qembd_pd->sound->sampleplayer->stop(v->player);
	qembd_pd->sound->sampleplayer->setSample(v->player, e->sample);
	v->bound = e->sample;
	qembd_pd->sound->sampleplayer->setVolume(v->player, left > 1 ? 1 : left, right > 1 ? 1 : right);
	qembd_pd->sound->sampleplayer->play(v->player, 1, 1.0f);
	v->entry = e;
	v->entnum = entnum;
	v->entchannel = entchannel;
	v->started = tick;
}

void S_StopSound(int entnum, int entchannel)
{
	for (int i = 0; i < NUM_PLAYERS; i++) {
		if (voices[i].entry && voices[i].entnum == entnum &&
		    voices[i].entchannel == entchannel) {
			qembd_pd->sound->sampleplayer->stop(voices[i].player);
			voices[i].entry = NULL;
		}
	}
}

void S_StopAllSounds(qboolean clear)
{
	for (int i = 0; i < NUM_PLAYERS; i++) {
		if (voices[i].player)
			qembd_pd->sound->sampleplayer->stop(voices[i].player);
		voices[i].entry = NULL;
	}
}

void S_LocalSound(char *s)
{
	sfx_t *sfx = S_PrecacheSound(s);

	if (!sfx) {
		Con_Printf("S_LocalSound: can't cache %s\n", s);
		return;
	}
	S_StartSound(cl.viewentity, 0, sfx, vec3_origin, 1, 1);
}

void S_Update(vec3_t origin, vec3_t v_forward, vec3_t v_right, vec3_t v_up)
{
	VectorCopy(origin, listener_origin);
	VectorCopy(v_right, listener_right);

	/* Release finished voices so their samples can be evicted */
	for (int i = 0; i < NUM_PLAYERS; i++)
		if (voices[i].entry &&
		    !qembd_pd->sound->sampleplayer->isPlaying(voices[i].player))
			voices[i].entry = NULL;
}

/* Not supported: ambient/looping sounds, CD music, the DMA mixer */
void S_StaticSound(sfx_t *sfx, vec3_t origin, float vol, float attenuation) {}
void S_AmbientOff(void) {}
void S_AmbientOn(void) {}
void S_TouchSound(char *sample) {}
void S_ClearBuffer(void) {}
void S_ClearPrecache(void) {}
void S_BeginPrecaching(void) {}
void S_EndPrecaching(void) {}
void S_ExtraUpdate(void) {}

/* sound_seq.cpp - the end of the sound-effect sequencer / SPU2 voice
 * manager (G002, 0x00268ED8..0x00269A68).  The rest of it (voice handles,
 * banks, sequence list, note-on) is in u123.
 *
 * Data: g_002FF168 voice bank (46 SPU2 voices), g_002FF958 the sequencer
 * (sequence list, master volume and fade, 47 sounding notes ("channels") at
 * 0x002FF970), g_00302698 the SPU2 state block sent to the IOP driver
 * (pdispu2.cxx) by snd_flush_voices.  A channel owns a voice through a
 * handle byte (SeqChannel.voice); the voice keeps a pointer back to that byte
 * and writes -1 into it when it is stolen or ends.
 */
/* PC: sound_seq.c of the decomp, compiled as C++ with the game's options;
 * unchanged but for the trace notes (GT3PC_SND_TRACE=2: snd_trace_lost when
 * a handle loses its voice) and the loop stack's bounds (seq_run_tracks).  snd_flush_voices's block goes to
 * pdispu2_send_state (T520), which now drives the native SPU2
 * (engine/sound/native). */
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/system/system_util_internal.h"
#include "engine/sound/spu_memory.h"            /* PC: snd_trace_lost */

void snd_trace_census(void);                    /* PC: below */

/* PC: the loop stack of a track (SeqTrack loopPos / loopCount).  The
 * original pushed a fifth nested loop past it (into the next track's
 * position and wait) and popped an empty one from below it; here a loop
 * deeper than the stack plays once (its start counted, not kept; its end
 * pops it) and an end with no loop open is ignored. */
#define SEQ_LOOP_STACK  ((int)(sizeof(((SeqTrack *)0)->loopCount) / sizeof(((SeqTrack *)0)->loopCount[0])))
#define SEQ_LOOP_DEPTH_MAX  0x7F                /* SeqTrack::loopDepth is an s8 */

// FUNC 0x00268ED8 seq_run_tracks
/* Run the 16 tracks of a sequence for seq->ticks ticks.  A track is a byte
 * stream of events, each followed by a delta time (MIDI-style variable length
 * quantity, 7 bits per byte, high bit = more):
 *   1nnnnnnn vel len(VLQ)   note-on: seq_note_on(seq, track, n, vel,
 *                           len + seq->ticks + track->wait)  (wait <= 0 here)
 *   01hhhhhh llllllll       pitch bend: ((h << 8) | l) - 0x2000
 *   001ccccc                loop start, count c + 1
 *   0x00                    end of track (pos = NULL, no delta follows)
 *   0x01 c                  loop start, count c
 *   0x02                    loop end: counter 0 -> pop, else decrement
 *                           (0xFF = forever) and jump back
 *   0x03 p                  track->program = p
 *   0x04 v                  track volume = g_00345F10[v], rescaled by volEff
 *   0x05 p                  track->pan = p
 *   0x06 t2 t1 t0           seq->tempo = 24-bit big-endian t
 *   0x07..0x1F              ignored (just the delta follows)
 * Events are executed while the track's wait is <= 0.  One variable `c'
 * holds the event byte, the note, the new depth of an 0x01 loop and the
 * delta, as the registers show ($a2; with a separate depth the two loop
 * openers get the same registers and jump2 merges their tails, which
 * retail does not); the new loop depth is not truncated to 8 bits before
 * the indexing (lb + 1).  u32 or int give the same code: u32 keeps a
 * 5-byte delta's shift defined in C.  The binary stores
 * track->pos back also when the track did not run (wait still > 0).  Only
 * caller seq_update. */
void seq_run_tracks(Sequence *seq)
{
    SeqTrack *track = seq->tracks;
    int i;

    for (i = 0; i < SEQ_NUM_TRACKS; i++, track++) {
        u8 *pos = track->pos;

        if (pos == NULL)
            continue;

        track->wait -= seq->ticks;
        if (track->wait <= 0) {
            do {
                u32 c = *pos++;

                if (c & SEQ_EV_NOTE) {
                    u32 velocity = *pos++;
                    u32 length;

                    c &= 0x7F;
                    length = *pos++;
                    if (length & SEQ_VLQ_MORE) {
                        u32 byte;

                        length &= 0x7F;
                        do {
                            byte = *pos++;
                            length = (length << 7) + (byte & 0x7F);
                        } while (byte & SEQ_VLQ_MORE);
                    }
                    seq_note_on(seq, track, c, velocity,
                                length + seq->ticks + track->wait);
                } else if (c & SEQ_EV_BEND) {
                    track->bend = (s16)((*pos++ | ((c & 0x3F) << 8)) - SEQ_BEND_CENTRE);
                } else if (c & SEQ_EV_LOOP_SHORT) {
                    int depth = track->loopDepth + 1;

                    if (depth < SEQ_LOOP_STACK) {   /* PC: kept only within the stack */
                        track->loopCount[depth] = (c & 0x1F) + 1;
                        track->loopPos[depth] = pos;
                    }
                    if (depth <= SEQ_LOOP_DEPTH_MAX)
                        track->loopDepth = depth;
                } else {
                    switch (c) {                /* jump table @0x00345DF0 (7 cases) */
                    case SEQ_OP_END:
                        pos = NULL;
                        goto store;
                    case SEQ_OP_LOOP:
                        c = track->loopDepth + 1;   /* the new depth (in c's register, $a2) */
                        if ((int)c < SEQ_LOOP_STACK) {  /* PC: kept only within the stack (its count read anyway) */
                            track->loopCount[c] = *pos;
                            track->loopPos[c] = pos + 1;
                        }
                        pos++;
                        if ((int)c <= SEQ_LOOP_DEPTH_MAX)
                            track->loopDepth = c;
                        break;
                    case SEQ_OP_LOOP_END:
                        if (track->loopDepth < 0) {
                            /* PC: no loop open: nothing (the original read below the stack) */
                        } else if (track->loopDepth >= SEQ_LOOP_STACK) {
                            track->loopDepth--;     /* PC: a loop past the stack ends after one pass */
                        } else if (track->loopCount[track->loopDepth] != 0) {
                            if (track->loopCount[track->loopDepth] != SEQ_LOOP_FOREVER)
                                track->loopCount[track->loopDepth]--;
                            pos = track->loopPos[track->loopDepth];
                        } else {
                            track->loopDepth--;
                        }
                        break;
                    case SEQ_OP_PROGRAM:
                        track->program = *pos++;
                        break;
                    case SEQ_OP_VOLUME:
                        track->volume = g_00345F10[*pos++];
                        track->volScaled = (seq->volEff * track->volume) >> SND_VOL_SHIFT;
                        break;
                    case SEQ_OP_PAN:
                        track->pan = *pos++;
                        break;
                    case SEQ_OP_TEMPO:
                        seq->tempo = (pos[0] << 16) | (pos[1] << 8) | pos[2];
                        pos += 3;
                        break;
                    default:                    /* 7..0x1F: nothing */
                        break;
                    }
                }

                /* delta time to the next event */
                c = *pos++;
                if (c & SEQ_VLQ_MORE) {
                    u32 byte;

                    c &= 0x7F;
                    do {
                        byte = *pos++;
                        c = (c << 7) + (byte & 0x7F);
                    } while (byte & SEQ_VLQ_MORE);
                }
                track->wait += c;
            } while (track->wait <= 0);
        }
    store:
        track->pos = pos;
    }
}

// FUNC 0x002691F0 seq_update
/* Per-frame sequencer step (caller music_thread), only while both
 * g_snd_ready (sound up) and g_seq_ready (sequencer initialised) are set.  For
 * every sequence: count how many times the tempo fits into the tick
 * accumulator (= ticks to run this frame), add the per-frame step, run the
 * tracks, then advance its volume fade.  Then the master fade (rescaling
 * every sequence), then the channel update (a tail call in the binary). */
void seq_update(void)
{
    Sequence *seq;

    if (g_snd_ready == 0 || g_seq_ready == 0)
        return;

    for (seq = g_002FF958.head; seq != NULL; seq = seq->next) {
        seq->ticks = 0;
        while (seq->tickAcc > 0) {
            seq->tickAcc -= seq->tempo;
            seq->ticks++;
        }
        seq->tickAcc += seq->frameStep;
        seq_run_tracks(seq);
        if (seq->fade.count != 0) {
            seq->volume += snd_ramp_step(&seq->fade);
            seq_update_volume(seq);
        }
    }

    if (g_002FF958.fade.count != 0) {
        g_002FF958.volume += snd_ramp_step(&g_002FF958.fade);
        for (seq = g_002FF958.head; seq != NULL; seq = seq->next)
            seq_update_volume(seq);
    }

    seq_update_channels();
}

// FUNC 0x00269328 seq_update_channels
/* Reached only by seq_update's tail jump.  For each of the 47 channels that
 * hold a voice: a channel whose sequence was stopped releases its voice
 * (key-off + free); a timed note counts down and keys off at <= 0; a changed
 * pitch bend re-sends the pitch (note in 8.8 semitones plus bend * range /
 * 32: the zone's range in semitones at full bend, chosen by the bend's
 * sign); a changed track volume or pan re-sends the volume:
 * gain = trackVol * program.vol * vtab[velocity] * zone.vol (each >> 14),
 * squared (>> 14) for the pan law, pan = zone.pan + track.pan + program.pan
 * - 0x80 clamped to 0..0x7F.  seq_note_on computes the same at the note's
 * start. */
void seq_update_channels(void)
{
    SndVolPair vol;                     /* sp+0x00 */
    SeqChannel *ch = g_002FF958.ch;
    int n;

    for (n = SEQ_NUM_CHANNELS - 1; n >= 0; n--, ch++) {
        if (ch->voice < 0)
            continue;

        if (ch->seq == NULL) {
            snd_voice_keyoff(&ch->voice);
            snd_voice_release(&ch->voice);
            continue;
        }

        if (ch->remain > 0) {
            ch->remain -= ch->seq->ticks;
            if (ch->remain <= 0)
                snd_voice_keyoff(&ch->voice);
        }

        if (ch->track->bend != ch->lastBend) {
            InstZone *zone = ch->zone;
            int range = (ch->track->bend > 0) ? zone->bend_up : zone->bend_down;

            snd_voice_set_pitch(&ch->voice,
                                snd_note_to_pitch((ch->note << 8) + ((range * ch->track->bend) >> 5),
                                                  zone->base_note));
            ch->lastBend = ch->track->bend;
        }

        if (ch->track->volScaled != ch->lastVol || ch->track->pan != ch->lastPan) {
            InstProgram *prog = ch->prog;
            InstZone *zone = ch->zone;
            int gain, pan;

            gain = (((((ch->track->volScaled * prog->vol) >> SND_VOL_SHIFT)
                     * g_00345F10[ch->velocity]) >> SND_VOL_SHIFT) * zone->vol) >> SND_VOL_SHIFT;
            pan = zone->pan + ch->track->pan + prog->pan;
            pan -= SND_PAN_BIAS;
            if (pan < 0)
                pan = 0;
            else if (pan > SND_PAN_MAX)
                pan = SND_PAN_MAX;
            snd_pan_volume(vol.lr, (gain * gain) >> SND_VOL_SHIFT, pan);
            snd_voice_set_volume(&ch->voice, &vol.word);
            ch->lastVol = ch->track->volScaled;
            ch->lastPan = ch->track->pan;
        }
    }
}

// FUNC 0x002694E8 seq_stop
/* Unlink a sequence and orphan its channels; seq_update_channels then
 * releases their voices.  Caller music_seq_stop. */
void seq_stop(Sequence *seq)
{
    SeqChannel *ch;
    int n;

    seq_unlink(seq);
    ch = g_002FF958.ch;
    for (n = SEQ_NUM_CHANNELS - 1; n >= 0; n--, ch++) {
        if (ch->seq == seq)
            ch->seq = NULL;
    }
}

// FUNC 0x00269530 snd_flush
/* Caller music_thread (once a frame). */
void snd_flush(void)
{
    if (g_snd_ready != 0)
        snd_flush_voices();             /* tail jump */
}

/* .sdata 0x003531B0 (this file's only small datum; u123's g_snd_ready /
 * g_seq_ready precede it). */
int g_snd_flush_count = 0;              /* 0x003531B0 snd_flush_voices calls (never read) */

// FUNC 0x00269550 snd_flush_voices
/* Build the voice part of the SPU2 state block from the 46 voices and send
 * it (tail call of pdispu2_send_state).  Runs with interrupts disabled from
 * after pdispu2_status (the voice-active mask of the last IOP answer, bit i
 * = voice i still sounding) until before the send.  Per voice:
 *   - playing, no key-on pending and its active bit clear: it ended by itself:
 *     key-off it; an allocated voice is freed and its owner's handle set -1;
 *   - key-off request: flag KEY_OFF if the voice is keyed on;
 *   - key-on request: KEY_OFF too when still keyed on (re-trigger), KEY_ON;
 *   - age counts flushes while playing;
 *   - unk_28 / unk_20 become flags 0xA0 / 0x100, the dirty bits PITCH /
 *     VOLUME / ADSR (0x200 / 0x400 / 0x800); dirty is cleared.
 * The flags keep their bits 0xF05C from the last block.  Pitch is converted
 * with * 0xEB3 / 4096 (= 44100 / 48000, signed division), the volumes are
 * masked to 15 bits, the address goes from 8-byte units to bytes.  Callers:
 * snd_flush (tail jump), snd_stop_all_voices (u123). */
void snd_flush_voices(void)
{
    SndVoiceBank *bank = &g_002FF168;
    u64 active[2];
    int intr_on;
    SndVoice *voice;
    SpuVoiceState *cmd;
    int i;

    /* PC: the voice pool (docs/sound.md "The engine"): the answer for its
     * 126 voices; voice i's state goes to the block's entry i for the PS2's
     * 46, to g_spuVoiceExt[i - 46] for the others (the block's entries
     * 46 / 47 are the stream's) */
    pdispu2_status128(active);
    intr_on = intr_suspend();
    g_snd_flush_count++;

    voice = bank->voices;
    for (i = 0; i < SND_NUM_VOICES; i++, voice++) {
        cmd = i < SND_VOICES_PS2 ? &g_00302698.voice[i] : &g_spuVoiceExt[i - SND_VOICES_PS2];
        int flags = cmd->flags;

        flags &= ~SPU_VOICE_REBUILT;

        if (voice->playing && voice->keyOnReq == 0 && !((active[i >> 6] >> (i & 63)) & 1)) {
            voice->playing = 0;
            voice->keyOffReq = 1;
            if (voice->state == SND_VOICE_ALLOCATED) {
                if (voice->owner != NULL) {
                    if (snd_trace_level() >= 2)     /* PC: the trace's note */
                        snd_trace_lost(voice->owner, SND_LOST_ENDED, i, voice->priority, 0);
                    *voice->owner = -1;
                }
                voice->owner = NULL;
                voice->state = SND_VOICE_FREE;
            }
        }
        if (voice->keyOffReq) {
            if (voice->keyedOn) {
                voice->keyedOn = 0;
                flags |= SPU_VOICE_KEY_OFF;
            }
            voice->keyOffReq = 0;
        }
        if (voice->keyOnReq) {
            if (voice->keyedOn)
                flags |= SPU_VOICE_KEY_OFF;
            voice->keyedOn = 1;
            voice->playing = 1;
            voice->keyOnReq = 0;
            flags |= SPU_VOICE_KEY_ON;
        }
        if (voice->playing)
            voice->age++;

        if (voice->unk_28)
            flags |= SPU_VOICE_UNK_A0;
        if (voice->unk_20)
            flags |= SPU_VOICE_UNK_100;
        if (voice->dirty & SND_DIRTY_PITCH)
            flags |= SPU_VOICE_PITCH;
        if (voice->dirty & SND_DIRTY_VOLUME)
            flags |= SPU_VOICE_VOLUME;
        if (voice->dirty & SND_DIRTY_ADSR)
            flags |= SPU_VOICE_ADSR;

        cmd->flags = flags;
        cmd->pitch = (int)(voice->pitch * SND_PITCH_44K_TO_48K) / SND_PITCH_ONE;
        cmd->volL = voice->volL & 0x7FFF;
        cmd->volR = voice->volR & 0x7FFF;
        cmd->addr = voice->addr << 3;
        cmd->adsr1 = voice->adsr1;
        cmd->adsr2 = voice->adsr2;
        voice->dirty = 0;
    }

    if (intr_on)
        EI();
    if (snd_trace_level() >= 2)         /* PC: the sound trace's census */
        snd_trace_census();
    pdispu2_send_state_ext(&g_00302698, g_spuVoiceExt, SND_VOICES_EXTRA);  /* tail jump (PC: with the pool's) */
}

/* PC: not the game's.  The sound trace (GT3PC_SND_TRACE=2, docs/sound.md
 * 3.6): every 60 flushes, the voice table as the EE sees it: free; owned
 * by a handle; ownerless and keyed on (fire-and-forget one-shots, or a
 * voice its owner forgot: those running for over 5 s are counted apart, a
 * leak would grow there); ownerless and keyed off (releasing, waiting for
 * the status); and the owned voices' priorities. */
void snd_trace_census(void)
{
    static int s_flushes = 0;
    SndVoice *voice = g_002FF168.voices;
    int freeN = 0, owned = 0, loose = 0, looseOld = 0, releasing = 0;
    int prioMin = 256, prioMax = -1, prio255 = 0;
    int i;

    if (++s_flushes < 60)
        return;
    s_flushes = 0;
    for (i = 0; i < SND_NUM_VOICES; i++, voice++) {
        if (voice->state == SND_VOICE_FREE) {
            freeN++;
        } else if (voice->owner != NULL) {
            owned++;
            if (voice->priority < prioMin)
                prioMin = voice->priority;
            if (voice->priority > prioMax)
                prioMax = voice->priority;
            if (voice->priority >= 0xFE)
                prio255++;
        } else if (voice->keyedOn) {
            loose++;
            if (voice->age > 300)
                looseOld++;
        } else {
            releasing++;
        }
    }
    snd_trace_line("EE voices: %d free, %d owned (priority %d..%d, %d at 254/255), %d ownerless keyed on "
                   "(%d of them for over 5 s), %d keyed off and releasing", freeN, owned,
                   prioMin > 255 ? -1 : prioMin, prioMax, prio255, loose, looseOld, releasing);
}

/* The file's .rodata after seq_run_tracks's jump table (0x00345DF0): a C
 * `const` table with an initialiser is written at its definition, so both
 * are defined after seq_run_tracks (values from the image). */

/* 0x4000 * sin(pi/2 * i / 127), rounded: the equal-power pan law
 * (snd_pan_volume). */
const u16 g_snd_pan_law[128] = {        /* 0x00345E10 */
    0x0000, 0x00CB, 0x0195, 0x0260, 0x032A, 0x03F5, 0x04BF, 0x0589,
    0x0653, 0x071C, 0x07E5, 0x08AE, 0x0977, 0x0A3F, 0x0B07, 0x0BCE,
    0x0C95, 0x0D5C, 0x0E22, 0x0EE7, 0x0FAC, 0x1070, 0x1133, 0x11F6,
    0x12B8, 0x137A, 0x143A, 0x14FA, 0x15B9, 0x1678, 0x1735, 0x17F1,
    0x18AD, 0x1967, 0x1A21, 0x1AD9, 0x1B91, 0x1C47, 0x1CFC, 0x1DB0,
    0x1E63, 0x1F15, 0x1FC5, 0x2075, 0x2123, 0x21CF, 0x227B, 0x2325,
    0x23CE, 0x2475, 0x251B, 0x25BF, 0x2662, 0x2703, 0x27A3, 0x2842,
    0x28DE, 0x2979, 0x2A13, 0x2AAB, 0x2B41, 0x2BD6, 0x2C68, 0x2CF9,
    0x2D89, 0x2E16, 0x2EA2, 0x2F2C, 0x2FB4, 0x303A, 0x30BE, 0x3141,
    0x31C1, 0x323F, 0x32BC, 0x3337, 0x33AF, 0x3426, 0x349A, 0x350C,
    0x357D, 0x35EB, 0x3657, 0x36C1, 0x3729, 0x378F, 0x37F2, 0x3853,
    0x38B3, 0x390F, 0x396A, 0x39C3, 0x3A19, 0x3A6D, 0x3ABE, 0x3B0D,
    0x3B5A, 0x3BA5, 0x3BED, 0x3C33, 0x3C77, 0x3CB8, 0x3CF7, 0x3D33,
    0x3D6E, 0x3DA5, 0x3DDA, 0x3E0D, 0x3E3E, 0x3E6C, 0x3E97, 0x3EC0,
    0x3EE7, 0x3F0B, 0x3F2D, 0x3F4C, 0x3F69, 0x3F83, 0x3F9B, 0x3FB0,
    0x3FC3, 0x3FD3, 0x3FE1, 0x3FEC, 0x3FF5, 0x3FFB, 0x3FFF, 0x4000
};

/* 0x4000 * i / 127, rounded: 7-bit volume / velocity -> gain (track volume
 * command, channel velocity; also u123's note-on). */
const u16 g_00345F10[128] = {           /* 0x00345F10 */
    0x0000, 0x0081, 0x0102, 0x0183, 0x0204, 0x0285, 0x0306, 0x0387,
    0x0408, 0x0489, 0x050A, 0x058B, 0x060C, 0x068D, 0x070E, 0x078F,
    0x0810, 0x0891, 0x0912, 0x0993, 0x0A14, 0x0A95, 0x0B16, 0x0B97,
    0x0C18, 0x0C99, 0x0D1A, 0x0D9B, 0x0E1C, 0x0E9D, 0x0F1E, 0x0F9F,
    0x1020, 0x10A1, 0x1122, 0x11A3, 0x1224, 0x12A5, 0x1326, 0x13A7,
    0x1428, 0x14A9, 0x152A, 0x15AB, 0x162C, 0x16AD, 0x172E, 0x17AF,
    0x1830, 0x18B1, 0x1932, 0x19B3, 0x1A34, 0x1AB5, 0x1B36, 0x1BB7,
    0x1C38, 0x1CB9, 0x1D3A, 0x1DBB, 0x1E3C, 0x1EBD, 0x1F3E, 0x1FBF,
    0x2041, 0x20C2, 0x2143, 0x21C4, 0x2245, 0x22C6, 0x2347, 0x23C8,
    0x2449, 0x24CA, 0x254B, 0x25CC, 0x264D, 0x26CE, 0x274F, 0x27D0,
    0x2851, 0x28D2, 0x2953, 0x29D4, 0x2A55, 0x2AD6, 0x2B57, 0x2BD8,
    0x2C59, 0x2CDA, 0x2D5B, 0x2DDC, 0x2E5D, 0x2EDE, 0x2F5F, 0x2FE0,
    0x3061, 0x30E2, 0x3163, 0x31E4, 0x3265, 0x32E6, 0x3367, 0x33E8,
    0x3469, 0x34EA, 0x356B, 0x35EC, 0x366D, 0x36EE, 0x376F, 0x37F0,
    0x3871, 0x38F2, 0x3973, 0x39F4, 0x3A75, 0x3AF6, 0x3B77, 0x3BF8,
    0x3C79, 0x3CFA, 0x3D7B, 0x3DFC, 0x3E7D, 0x3EFE, 0x3F7F, 0x4000
};

// FUNC 0x00269750 snd_pan_volume
/* out[0] (left) = vol * pan_law[0x7F - pan] >> 14, out[1] (right) =
 * vol * pan_law[pan] >> 14 (equal power: the law is a quarter sine).
 * Callers: se_play_program, seq_note_on, seq_update_channels. */
void snd_pan_volume(s16 *out, int vol, int pan)
{
    out[0] = (s16)((vol * g_snd_pan_law[SND_PAN_MAX - pan]) >> SND_VOL_SHIFT);
    out[1] = (s16)((vol * g_snd_pan_law[pan]) >> SND_VOL_SHIFT);
}

// FUNC 0x00269798 snd_voice_alloc
/* Find a voice for a sound of the given priority, with interrupts disabled.
 * The first free voice is taken at once.  Otherwise the allocated voice with
 * the largest priority value >= the requested one is stolen, the oldest
 * (largest age, unsigned) among equals: its owner's handle is set to -1 and
 * a key-off is requested.  The chosen voice gets the new priority.  Returns
 * its index, -1 if none qualifies.  Caller snd_play_voice. */
int snd_voice_alloc(int priority)
{
    int intr_on = intr_suspend();
    SndVoiceBank *bank = &g_002FF168;
    SndVoice *voice = bank->voices;
    SndVoice *best = NULL;
    int bestIdx = -1;
    int bestPrio = priority;
    u32 bestAge = 0;
    int i;

    for (i = 0; i < SND_NUM_VOICES; i++, voice++) {
        switch (voice->state) {
        case SND_VOICE_FREE:
            best = voice;
            bestIdx = i;
            goto take;
        case SND_VOICE_ALLOCATED: {
            int prio = voice->priority;

            if (prio < bestPrio)
                continue;
            if (bestPrio == prio && voice->age < bestAge)
                continue;
            best = voice;
            bestPrio = prio;
            bestAge = best->age;
            bestIdx = i;
            break;
        }
        }
    }

    if (best != NULL) {
        /* (declared without an initialiser: the goto take above jumps
         * past it, which C++ allows only then) */
        s8 *owner;

        owner = best->owner;
        if (owner != NULL) {
            if (snd_trace_level() >= 2)             /* PC: the trace's note */
                snd_trace_lost(owner, SND_LOST_STOLEN, bestIdx, best->priority, priority);
            best->owner = NULL;
            *owner = -1;
        }
        best->dirty = 0;
        best->keyOffReq = 1;
        best->keyOnReq = 0;
    take:
        best->priority = priority;
    }

    if (intr_on)
        EI();
    return bestIdx;
}

// FUNC 0x00269890 snd_voice_start
/* Load a voice from p and request its key-on (interrupts disabled): volume
 * and ADSR are copied as whole words, all dirty bits set, age 0.
 * unk_20 = g_002FF168.unk_06 * p->unk_10.  Sets g_002FF168.updated.
 * Retail runs the stores in this order: the word stores are union members
 * (SND_HALF_PAIR), which egcs never moves across other accesses.
 * Caller snd_play_voice (which has set state and owner). */
void snd_voice_start(int idx, const SndVoiceParam *p)
{
    int intr_on = intr_suspend();
    SndVoiceBank *bank = &g_002FF168;
    SndVoice *voice = &bank->voices[idx];

    voice->playing = 0;
    voice->priority = p->priority;
    voice->dirty = SND_DIRTY_ALL;
    voice->keyOnReq = 1;
    voice->keyOffReq = 0;
    SND_HALF_PAIR(voice->volL) = p->vol.word;  /* sw: volL | volR << 16 */
    voice->addr = p->addr;
    voice->pitch = p->pitch;
    SND_HALF_PAIR(voice->adsr1) = p->adsr;      /* sw: adsr1 | adsr2 << 16 */
    voice->age = 0;
    voice->unk_24 = 0;
    voice->unk_20 = bank->unk_06 * p->unk_10;
    voice->unk_28 = p->flags & 1;
    bank->updated = 1;

    if (intr_on)
        EI();
}

// FUNC 0x00269948 snd_ramp_init
/* Set up a ramp from `from` to `to` over ms milliseconds:
 * frames = ms * g_002FF958.frameRate / 1000 (at least 1), step =
 * delta / frames, rem = |delta| % frames, extra = sign(delta) when rem != 0,
 * err = frames / 2.  delta == 0 only clears count.  Callers: seq_fade,
 * seq_master_fade (tail jumps). */
void snd_ramp_init(SndRamp *r, int from, int to, int ms)
{
    int delta = to - from;
    int neg, frames, step, rem, extra;

    if (delta == 0) {
        r->count = 0;
        return;
    }
    neg = 0;
    if (delta <= 0) {
        neg = 1;
        delta = -delta;
    }
    frames = (ms * g_002FF958.frameRate) / 1000;
    if (frames == 0)
        frames = 1;
    step = delta / frames;
    rem = delta % frames;
    extra = (rem != 0);
    if (neg) {
        step = -step;
        extra = -extra;
    }
    r->step = step;
    r->extra = extra;
    r->rem = rem;
    r->err = frames >> 1;
    r->denom = frames;
    r->count = frames;                  /* last: arms the ramp */
}

// FUNC 0x002699D0 snd_ramp_step
/* One frame of a ramp: returns the value to add (0 once count is 0).
 * Quirk: the correction test reads the sign of rem (bit 15 of +0x06), not of
 * the decremented error term, so with rem < 0x8000 (always for ramps under
 * 0x8000 frames) the +-1 correction never happens and the ramp ends short of
 * its target by up to rem; err still counts down.  Only caller seq_update. */
int snd_ramp_step(SndRamp *r)
{
    int add;

    if (r->count == 0)
        return 0;
    r->count--;
    add = r->step;
    if (r->extra != 0) {
        r->err -= r->rem;
        if ((s16)r->rem < 0) {          /* sic: rem, not err */
            add += r->extra;
            r->err += r->denom;
        }
    }
    return add;
}

// FUNC 0x00269A28 seq_fade
/* Fade a sequence's volume to `to` over ms milliseconds.  Unused: no jal,
 * j or pointer to it anywhere in the image (the only way snd_ramp_init is
 * reached, so sequence fades never run). */
void seq_fade(Sequence *seq, u16 to, int ms)
{
    seq->fade.count = 0;
    snd_ramp_init(&seq->fade, seq->volume, to, ms);     /* tail jump */
}

// FUNC 0x00269A48 seq_master_fade
/* Fade the master volume.  Unused like seq_fade. */
void seq_master_fade(u16 to, int ms)
{
    g_002FF958.fade.count = 0;
    snd_ramp_init(&g_002FF958.fade, g_002FF958.volume, to, ms);   /* tail jump */
}

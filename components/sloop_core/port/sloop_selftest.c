/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: a deterministic render of the SLOOP engine for cross-target checks.
 *
 * Four tracks of a fixed song (as upstream tests/hostsim.c's TRACKS demo: an ANALOG acid line,
 * a DIGITAL pad in tied chords, a LOFI lead in 3 against 4, a drum pattern with accents) at
 * 120 BPM, rendered block by block with mix_block, no clock, no UI, no input. The host tests
 * and the ESP32 firmware (QEMU) run the same function: equal hashes mean the cross-compiled
 * DSP, sequencer and mixer produce bit-identical audio. Included by sloop_unity.c.
 * Call on a fresh power-on, before sloop_boot (it sets the whole song state).
 */
static void st_put_step(track_t *t, uint32_t i, uint32_t n, const uint8_t *notes, uint32_t time, uint32_t flags)
{
    step_t *s = &t->step[i];
    uint32_t k;
    if (is_drum(t)) {
        memset(&t->dstep[i], 0, sizeof t->dstep[i]);
        for (k = 0; k < n && time == ST_NOTE; k++)
            dstep_set(&t->dstep[i], lane_of_note(notes[k]), (flags & SF_ACCENT) ? LV_HARD : LV_NORM, 0);
        return;
    }
    s->n = (uint8_t)n;
    for (k = 0; k < 4u; k++)
        s->note[k] = k < n ? notes[k] : 0;
    s->time = (uint8_t)time;
    s->flags = (uint8_t)flags;
    s->vel = n ? 100 : 0;
}

/* the factory preset as ui.c apply_preset_to sets it (sound, sends, arp) */
static void st_preset(track_t *t, uint32_t e, uint32_t pi)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    const preset_t *p = &ENGINES[e]->presets[pi % ENGINES[e]->npresets];
    uint32_t i;
    t->eng_req = (uint8_t)e;
    t->engine = (uint8_t)e;
    t->preset = (uint8_t)(pi % ENGINES[e]->npresets);
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = p->e[i];
    t->p[P_ATK] = p->env[0];
    t->p[P_DEC] = p->env[1];
    t->p[P_SUS] = p->env[2];
    t->p[P_REL] = p->env[3];
    t->p[P_ED_FLT] = p->fenv;
    t->p[P_ED_FX] = preset_trim(e, pi % ENGINES[e]->npresets);
    t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
    for (i = 0; i < 4u; i++) {
        t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : FX_DEF[i]);
        t->p[P_AMODE + i] = (int16_t)(p->arp[i] ? p->arp[i] - 1 : TP[P_AMODE + i].def);
    }
    preset_extras(t->p, p);
}

uint64_t sloop_selftest_render(uint32_t ms, int32_t *peak_out)
{
    static const uint8_t ACID[16] = {45, 45, 57, 45, 0, 48, 45, 55, 45, 0, 57, 52, 45, 48, 0, 50};
    static const uint8_t ACIDF[16] = {1, 0, 2, 0, 0, 0, 1, 2, 0, 0, 1, 0, 0, 2, 0, 1};
    static const uint8_t AM[4] = {57, 60, 64, 67}, FM[4] = {53, 57, 60, 64};
    static const uint8_t LEAD[12] = {76, 0, 0, 79, 0, 0, 81, 0, 79, 0, 76, 0};
    static int32_t o[2 * CTL];
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t i, k, f, frames = (uint32_t)((uint64_t)ms * FS / 1000u);
    int32_t peak = 0;
    track_t *t1 = &trk[0], *t2 = &trk[1], *t3 = &trk[2], *td = TDRUM;

    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    song.g[G_BPM] = 120;
    for (k = 0; k < NTRK; k++) {
        for (i = 0; i < P_E0; i++)
            trk[k].p[i] = TP[i].def;
        steps_clear(&trk[k]);
    }
    song.master_q12 = 4096;
    st_preset(t1, 0, 4);
    st_preset(t2, 1, 5);
    st_preset(t3, 3, 0);
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    for (i = 0; i < 16u; i++) {
        uint8_t n = ACID[i];
        st_put_step(t1, i, n ? 1u : 0u, &n, n ? ST_NOTE : ST_REST, ACIDF[i]);
    }
    t2->p[P_SLEN] = 32;
    t2->p[P_SGATE] = 120;
    for (i = 0; i < 32u; i++)
        st_put_step(t2, i, i % 16u == 0u ? 4u : 0u, i < 16u ? AM : FM,
                    i % 16u == 0u ? ST_NOTE : i % 16u < 14u ? ST_TIE : ST_REST, 0);
    t3->p[P_SLEN] = 12;
    for (i = 0; i < 12u; i++) {
        uint8_t n = LEAD[i];
        st_put_step(t3, i, n ? 1u : 0u, &n, n ? ST_NOTE : ST_REST, i == 0u ? SF_ACCENT : 0u);
    }
    for (i = 0; i < 16u; i++) {                    /* kick 4 on the floor, snare 4 / 12, hats on the 8ths */
        uint8_t n[4];
        k = 0;
        if (i % 4u == 0u)
            n[k++] = 36;
        if (i == 4u || i == 12u)
            n[k++] = 38;
        if (i % 2u == 0u)
            n[k++] = i == 14u ? 46 : 42;
        st_put_step(td, i, k, n, k ? ST_NOTE : ST_REST, i % 4u == 0u ? SF_ACCENT : 0u);
    }
    transport_req = 1;
    for (f = 0; f < frames; f += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            uint32_t u = (uint32_t)o[i];
            int32_t a = o[i] < 0 ? -o[i] : o[i];
            if (a > peak)
                peak = a;
            h = (h ^ (u & 0xFFu)) * 0x100000001B3ull;
            h = (h ^ ((u >> 8) & 0xFFu)) * 0x100000001B3ull;
            h = (h ^ ((u >> 16) & 0xFFu)) * 0x100000001B3ull;
            h = (h ^ (u >> 24)) * 0x100000001B3ull;
        }
    }
    transport_req = 2;
    mix_block(o, CTL);
    if (peak_out)
        *peak_out = peak;
    return h;
}

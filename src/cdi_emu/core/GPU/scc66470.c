/* SCC66470 register and timing emulation; deliberately has no SDL dependency. */
#include "scc66470.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICA_POINTER    0x00400u   /* 256K DRAM, bank 1 enabled (Table 8) */

static int video_log_enabled(void)
{
    static int state = -1;
    if (state < 0) {
        const char *s = getenv("EMU_VIDEO_LOG");
        state = (s && *s && strcmp(s, "0") != 0) ? 1 : 0;
    }
    return state;
}

static void video_log(const char *op, uint32_t addr, uint16_t value)
{
    if (video_log_enabled())
        fprintf(stderr, "SCC66470 %s %06X = %04X\n", op,
                (unsigned)(addr & 0xFFFFFFu), (unsigned)value);
}

/* Power-on state: all registers cleared (datasheet: CSR and DCR bits 4-15
   are reset to zero). The CLUT gets a grey ramp so 8-bit pixel values are
   visible until a real palette device is emulated. */
void scc66470_init(scc66470_t *v)
{
    memset(v, 0, sizeof(*v));
    v->ica_pointer = ICA_POINTER;
    for (int i = 0; i < 256; i++)
        v->clut[i] = 0xFF000000u | ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
}

uint8_t scc66470_status(const scc66470_t *v)
{
    uint64_t pos = v->cycle_counter % SCC_CYCLES_PER_FRAME;
    return (uint8_t)((pos < SCC_ACTIVE_CYCLES) ? SCC_CSR_DA : 0u);
}

static uint16_t scc_peek16(const scc66470_t *v, uint32_t addr)
{
    switch (addr) {
    case SCC_C0: return v->reg_c0; case SCC_C2: return v->reg_c2; case SCC_C8: return v->reg_c8;
    case SCC_CSR:
        /* Return CSR with Display Active bit set (bit 7) as per hardware */
        /* TODO: per the datasheet a read returns only the status bits
         * (DA FG PA - - IT2 IT1 BE); the low control bits are echoed here
         * because the boot code currently relies on this behaviour. */
        return (uint16_t)(scc66470_status(v) | (v->csr_control & 0x007Du) | (v->it1 ? 0x02u : 0u));
    case SCC_DCR: return v->dcr; case SCC_VSR: return v->vsr; case SCC_BCR: return v->bcr;
    case SCC_DCR2: return v->dcr2; case SCC_DCP: return v->dcp; case SCC_SWM: return v->swm;
    case SCC_A: return v->a; case SCC_B: return v->b; case SCC_PCR: return v->pcr;
    case SCC_MASK: return v->mask; case SCC_SHIFT: return v->shift;
    case SCC_INDEX: return v->index; case SCC_FC: return v->fc; case SCC_BC: return v->bc; case SCC_TC: return v->tc;
    default: return 0u;
    }
}

static int is_native8(uint32_t addr)
{
    return addr == SCC_INDEX || addr == SCC_FC || addr == SCC_BC || addr == SCC_TC;
}


int scc66470_tick(scc66470_t *v, int cpu_cycles)
{
    if (cpu_cycles <= 0) return 0;
    uint64_t c = v->cycle_counter + (uint64_t)(unsigned)cpu_cycles;
    v->cycle_counter = c % SCC_CYCLES_PER_FRAME;
    return c >= SCC_CYCLES_PER_FRAME;
}

int scc66470_irq(const scc66470_t *v)
{
    return v->it1 != 0;
}

uint8_t scc66470_read8(scc66470_t *v, uint32_t addr)
{
    if (addr == SCC_CSR + 1) {                 /* status byte: reading clears IT1 */
        uint8_t r = (uint8_t)scc_peek16(v, SCC_CSR);
        v->it1 = 0;
        return r;
    }
    /* These four locations are physically 8-bit registers, not byte lanes
       of a surrounding 16-bit register. */
    if (is_native8(addr)) {
        switch (addr) {
        case SCC_INDEX: return v->index;
        case SCC_FC:    return v->fc;
        case SCC_BC:    return v->bc;
        case SCC_TC:    return v->tc;
        }
    }
    {
        uint16_t word = scc_peek16(v, addr & ~1u);
        return (addr & 1u) ? (uint8_t)word : (uint8_t)(word >> 8);
    }
}

uint16_t scc66470_read16(scc66470_t *v, uint32_t addr)
{
    if (addr == SCC_CSR) {                     /* reading the status clears IT1 */
        uint16_t r = scc_peek16(v, addr);
        v->it1 = 0;
        return r;
    }
    return scc_peek16(v, addr);
}

void scc66470_write16(scc66470_t *v, uint32_t addr, uint16_t val)
{
    video_log("W16", addr, val);
    switch (addr) {
    case SCC_C0: v->reg_c0 = val; break; case SCC_C2: v->reg_c2 = val; break; case SCC_C8: v->reg_c8 = val; break;
    case SCC_CSR:
        /* Control register (write): DI1 DI2 - - - EW DD1 DD2 DM1 DM2 TD CG DD ED ST BE.
         * Kept whole: DM1/DM2 (FAST/SLOW timing) and ST are needed for display. */
        v->csr_control = val;
        break;
    case SCC_DCR: v->dcr = val; break; case SCC_VSR: v->vsr = val; break;
    case SCC_BCR: v->bcr = val; break; case SCC_DCR2: v->dcr2 = val; break; case SCC_DCP: v->dcp = val; break;
    case SCC_SWM: v->swm = val; break; case SCC_A: v->a = val; break; case SCC_B: v->b = val; break;
    case SCC_PCR: v->pcr = val; break; case SCC_MASK: v->mask = val; break; case SCC_SHIFT: v->shift = val; break;
    case SCC_INDEX: v->index = (uint8_t)val; break; case SCC_FC: v->fc = (uint8_t)val; break;
    case SCC_BC: v->bc = (uint8_t)val; break; case SCC_TC: v->tc = (uint8_t)val; break;
    default: break;
    }
}

void scc66470_write8(scc66470_t *v, uint32_t addr, uint8_t val)
{
    if (is_native8(addr)) {
        switch (addr) { case SCC_INDEX: v->index = val; break; case SCC_FC: v->fc = val; break;
        case SCC_BC: v->bc = val; break; case SCC_TC: v->tc = val; break; }
        video_log("W8", addr, val);
        return;
    }
    /* The bus is big-endian: even address is the high byte, odd is the low byte. */
    uint32_t base = addr & ~1u;
    uint16_t old = scc_peek16(v, base);
    uint16_t word = (addr & 1u) ? (uint16_t)((old & 0xFF00u) | val)
                               : (uint16_t)(((uint16_t)val << 8) | (old & 0x00FFu));
    scc66470_write16(v, base, word);
}

void scc66470_set_clut_entry(scc66470_t *v, int index, uint16_t rgb555)
{
    if (index >= 0 && index < 256) {
        uint8_t r = (uint8_t)(((rgb555 >> 11) & 31u) << 3);
        uint8_t g = (uint8_t)(((rgb555 >> 6) & 31u) << 3);
        uint8_t b = (uint8_t)(((rgb555 >> 1) & 31u) << 3);
        v->clut[index] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

/* ======================================================================
 * Display generation
 *
 * Implements the display path of the SCC66470/03 datasheet:
 *   - DCR:  DE (15) CF1 CF2 FD SM SS LS CM (8) FG DF IC DC A19..A16
 *   - DCR2: OM1 (14) OM2 ID MF1 MF2 FT1 FT2 (8) .... A19..A16 (DCA ptr)
 *   - CSR (control): DM1 (7) DM2 (6) -> FAST/SLOW timing, ST (1)
 *   - ICA: control program fetched from $400 at each vertical retrace
 *   - DCA: per-line control program (interleaved or independent)
 *   - file types: bit map, run-length, mosaic; 4 or 8 bits per pixel
 * ====================================================================== */

#define VA_MASK        0xFFFFFu   /* the VSC addresses 1 MB of DRAM */
#define ICA_MAX_INSNS  2048       /* safety bound on one ICA program */

/* Working display state, modified by ICA/DCA instructions */
typedef struct {
    uint32_t vac;      /* video address counter (VSR + A19..A16) */
    uint32_t dcp;      /* DCA pointer */
    uint8_t  bcr;      /* border colour (8-bit pixel value) */
    int ss, cm, om1, om2, mf1, mf2, ft1, ft2;
    int dcp_reloaded;  /* a RELOAD DCP happened during this DCA */
    int interrupt;     /* an INTERRUPT instruction was executed */
    int line;          /* output row the BEP words apply from (-1 = ICA) */
    scc_frame_t *out;
} disp_state_t;

typedef struct {
    const uint8_t *mem;
    uint32_t       mask;
} vram_t;

static inline uint8_t vr8(const vram_t *m, uint32_t a) { return m->mem[a & m->mask]; }
static inline uint32_t vr32(const vram_t *m, uint32_t a)
{
    return ((uint32_t)vr8(m, a) << 24) | ((uint32_t)vr8(m, a + 1) << 16) |
           ((uint32_t)vr8(m, a + 2) << 8) | vr8(m, a + 3);
}

enum { CTX_ICA, CTX_DCA_INDEP, CTX_DCA_INTERLEAVED };

/*
 * Execute an ICA/DCA program (Table 11). Returns the number of
 * instructions executed. In the ICA and in an interleaved DCA, a RELOAD
 * VSR also redirects the instruction fetches (indirect addressing).
 */
static int run_control(disp_state_t *s, const vram_t *m, uint32_t ptr,
                       int max_insns, int ctx)
{
    int n;
    for (n = 0; n < max_insns; n++) {
        uint32_t w = vr32(m, ptr);
        ptr = (ptr + 4) & VA_MASK;

        if (w & 0x80000000u) {                     /* BEP CONTROL */
            if (s->out->bep_count < SCC_BEP_MAX) {
                s->out->bep_line[s->out->bep_count] = (int16_t)s->line;
                s->out->bep[s->out->bep_count++] = w;
            }
            continue;
        }
        switch (w >> 28) {
        case 0x0: return n + 1;                    /* STOP */
        case 0x1: break;                           /* NOP */
        case 0x2:                                  /* RELOAD DCP */
        case 0x3:                                  /* RELOAD DCP and STOP */
            s->dcp = w & 0xFFFFCu;
            s->dcp_reloaded = 1;
            if ((w >> 28) == 0x3) return n + 1;
            if (ctx == CTX_DCA_INDEP) ptr = s->dcp;
            break;
        case 0x4:                                  /* RELOAD VSR */
        case 0x5:                                  /* RELOAD VSR and STOP */
            s->vac = w & VA_MASK;
            if ((w >> 28) == 0x5) return n + 1;
            if (ctx != CTX_DCA_INDEP) ptr = s->vac;
            break;
        case 0x6: s->interrupt = 1; break;         /* INTERRUPT: sets IT1 */
        case 0x7:                                  /* RELOAD BORDER [+ DISPLAY] */
            s->bcr = (uint8_t)(w >> 16);
            if (w & 0x08000000u) {
                s->ss  = (w >> 10) & 1;  s->cm  = (w >> 8) & 1;
                s->om1 = (w >> 6) & 1;   s->om2 = (w >> 5) & 1;
                s->mf1 = (w >> 3) & 1;   s->mf2 = (w >> 2) & 1;
                s->ft1 = (w >> 1) & 1;   s->ft2 = w & 1;
            }
            break;
        }
    }
    return n;
}

/* Map a 4-bit pixel to the V(7:0) port according to the output mode (Table 12) */
static inline uint8_t out4(const disp_state_t *s, uint8_t nib)
{
    nib &= 0x0F;
    if (!s->om1 && s->om2) return nib;            /* mode E: V3..V0 */
    if (s->om1 && !s->om2)                        /* mode F: V7 V6 V4 V3 V1 V0 */
        return (uint8_t)(((nib & 8) ? 0xC0 : 0) | ((nib & 4) ? 0x18 : 0) |
                         ((nib & 2) ? 0x02 : 0) | ((nib & 1) ? 0x01 : 0));
    return (uint8_t)(nib << 4);                   /* mode D (default): V7..V4 */
}

/* Horizontal full/reduced resolutions in 4-bit pixels (Table 4, CM = 1) */
static void h_res(int cf, int st, int *full, int *reduced)
{
    static const int tab_red[4]  = { 448, 512, 640, 640 };
    static const int tab_full[4] = { 512, 640, 720, 768 };
    *reduced = tab_red[cf];
    *full    = tab_full[cf];
    if (cf == 3 && st) *full = 720;               /* ST masks 24 pixels at 30 MHz */
}

/* Vertical full/reduced line counts (Table 5) */
static void v_res(int fd, int st, int *full, int *reduced)
{
    if (fd) { *full = 240; *reduced = 210; return; }      /* 60 Hz */
    *full    = st ? 240 : 280;
    *reduced = st ? 210 : 250;
}

/*
 * Render one display line of npix source pixels starting at the video
 * address counter, writing unit output pixels per 4-bit pixel.
 */
static void render_line(disp_state_t *s, const vram_t *m, uint8_t *dst,
                        int npix, int unit)
{
    int mosaic = 1;
    if (s->ft1 && s->ft2) mosaic = 2 << ((s->mf1 << 1) | s->mf2);
    int pw = s->cm ? unit : unit * 2;             /* output pixels per source pixel */
    int x = 0;

    if (s->ft1 && !s->ft2) {
        /* ---- Run-length file (Figs. 21-24) ---- */
        while (x < npix) {
            uint8_t b = vr8(m, s->vac); s->vac = (s->vac + 1) & VA_MASK;
            int count = 1;
            if (b & 0x80) {                       /* type B: colour + N */
                count = vr8(m, s->vac); s->vac = (s->vac + 1) & VA_MASK;
                if (count == 0) count = npix;     /* N = 0: until end of line */
            }
            for (int k = 0; k < count && x < npix; k++) {
                if (s->cm) {                       /* 4 bpp: two 3-bit colours */
                    uint8_t p[2] = { (uint8_t)((b >> 4) & 7), (uint8_t)(b & 7) };
                    for (int h = 0; h < 2 && x < npix; h++, x++)
                        for (int u = 0; u < pw; u++) dst[x * pw + u] = out4(s, p[h]);
                } else {                           /* 8 bpp: 7-bit colour */
                    for (int u = 0; u < pw; u++) dst[x * pw + u] = (uint8_t)(b & 0x7F);
                    x++;
                }
            }
        }
        return;
    }

    /* ---- Bit map / mosaic file (Figs. 19, 20, 25) ---- */
    while (x < npix) {
        uint8_t b = vr8(m, s->vac); s->vac = (s->vac + 1) & VA_MASK;
        if (s->cm) {
            uint8_t p[2] = { out4(s, b >> 4), out4(s, b) };
            for (int h = 0; h < 2; h++)
                for (int r = 0; r < mosaic && x < npix; r++, x++)
                    for (int u = 0; u < pw; u++) dst[x * pw + u] = p[h];
        } else {
            for (int r = 0; r < mosaic && x < npix; r++, x++)
                for (int u = 0; u < pw; u++) dst[x * pw + u] = b;
        }
    }
}

void scc66470_render(const scc66470_t *v, const uint8_t *dram, uint32_t dram_size,
                     scc_frame_t *out)
{
    vram_t m = { dram, (dram_size >= 0x100000u) ? VA_MASK : (dram_size - 1u) };
    disp_state_t s;
    memset(&s, 0, sizeof(s));
    s.out = out;
    out->bep_count = 0;
    out->ica_count = 0;

    uint16_t dcr = v->dcr, dcr2 = v->dcr2, csr = v->csr_control;
    out->enabled = (dcr >> 15) & 1;

    /* Initial state from the registers */
    s.vac = (((uint32_t)dcr & 0xFu) << 16) | v->vsr;
    s.dcp = ((((uint32_t)dcr2 & 0xFu) << 16) | v->dcp) & 0xFFFFCu;
    s.bcr = (uint8_t)v->bcr;
    s.ss  = (dcr >> 10) & 1;  s.cm  = (dcr >> 8) & 1;
    s.om1 = (dcr2 >> 14) & 1; s.om2 = (dcr2 >> 13) & 1;
    s.mf1 = (dcr2 >> 11) & 1; s.mf2 = (dcr2 >> 10) & 1;
    s.ft1 = (dcr2 >> 9) & 1;  s.ft2 = (dcr2 >> 8) & 1;

    int ic = (dcr >> 5) & 1, dc = (dcr >> 4) & 1;
    int ls = (dcr >> 9) & 1;
    int id = (dcr2 >> 12) & 1;
    int cf = (dcr >> 13) & 3;
    int fd = (dcr >> 12) & 1;
    int st = (csr >> 1) & 1;
    int dm = (csr >> 6) & 3;                       /* DM1 DM2 */
    int slow = (dm == 0 || dm == 3);               /* NORMAL / DUAL PORT */

    /* ICA: vertical retrace control program */
    s.line = -1;
    if (ic || dc)
        out->ica_count = run_control(&s, &m, v->ica_pointer ? v->ica_pointer : ICA_POINTER,
                                     ICA_MAX_INSNS, CTX_ICA);
    out->vsr_start = s.vac;

    /* Geometry (uses SS after the ICA, which may have changed it) */
    int hfull, hred, vfull, vred;
    h_res(cf, st, &hfull, &hred);
    v_res(fd, st, &vfull, &vred);
    out->width  = hfull;
    out->height = vfull;

    int act_w = s.ss ? hfull : hred;
    int act_h = s.ss ? vfull : vred;
    int left  = (hfull - act_w) / 2;
    int top   = (vfull - act_h) / 2;
    int unit  = slow ? 2 : 1;                      /* SLOW halves the resolution */

    /* DCA size (Tables 9/10): 64 bytes with ICA, 16 bytes in reduced mode */
    int dca_insns = dc ? (ic ? 16 : 4) : 0;
    if (slow) dca_insns /= 2;

    /* Fill with the border colour */
    for (int y = 0; y < vfull; y++)
        memset(&out->pix[y * SCC_OUT_MAX_W], s.bcr, (size_t)hfull);

    uint32_t line_start = s.vac;
    for (int y = 0; y < act_h; y++) {
        uint8_t *row = &out->pix[(top + y) * SCC_OUT_MAX_W];

        /* Independent DCA: executed before the line, at DCP */
        s.line = top + y;
        if (dca_insns && id) {
            uint32_t this_dcp = s.dcp;
            uint32_t before   = s.vac;
            s.dcp_reloaded = 0;
            run_control(&s, &m, this_dcp, dca_insns, CTX_DCA_INDEP);
            if (!s.dcp_reloaded) s.dcp = (this_dcp + (uint32_t)dca_insns * 4u) & VA_MASK;
            if (s.vac != before) line_start = s.vac;
        }

        /* Border colour may have been changed by the DCA */
        memset(row, s.bcr, (size_t)hfull);

        int pw   = s.cm ? unit : unit * 2;
        int npix = act_w / pw;
        s.vac = line_start;
        render_line(&s, &m, row + left, npix, unit);

        uint32_t next = ls ? ((line_start + 512u) & VA_MASK) : s.vac;

        /* Interleaved DCA: stored after the line pixels (Fig. 10) */
        if (dca_insns && !id) {
            uint32_t dca_addr = ls ? ((line_start & ~0x1FFu) | (dca_insns == 16 ? 0x1C0u : 0x1F0u))
                                   : s.vac;
            uint32_t before = s.vac;
            s.line = top + y + 1;                  /* after the pixels: next line */
            run_control(&s, &m, dca_addr, dca_insns, CTX_DCA_INTERLEAVED);
            if (s.vac != before) next = s.vac;    /* RELOAD VSR in the DCA */
            else if (!ls) next = (dca_addr + (uint32_t)dca_insns * 4u) & VA_MASK;
        }
        line_start = next;
    }
    out->interrupt = s.interrupt;
}

/* Vertical retrace: the frame's control programs run; an INTERRUPT
 * instruction sets IT1 (unless DI1 = CSR bit 15). Besides the ICA, the
 * line DCAs count: CD-RTOS puts an INTERRUPT in the DCA of a given line
 * for a "signal at line N" request (Inca waits for it after its intro).
 * The interrupt is raised once per frame, at the retrace. */
void scc66470_vretrace(scc66470_t *v, const uint8_t *dram, uint32_t dram_size)
{
    uint16_t dcr = v->dcr;
    if (!((dcr >> 5) & 1) && !((dcr >> 4) & 1)) return;     /* no IC / DC */
    vram_t m = { dram, (dram_size >= 0x100000u) ? VA_MASK : (dram_size - 1u) };
    scc_frame_t scratch;
    disp_state_t st;
    memset(&st, 0, sizeof(st));
    scratch.bep_count = 0;
    st.out = &scratch;
    st.vac = (((uint32_t)dcr & 0xFu) << 16) | v->vsr;
    st.dcp = ((((uint32_t)v->dcr2 & 0xFu) << 16) | v->dcp) & 0xFFFFCu;
    run_control(&st, &m, v->ica_pointer ? v->ica_pointer : ICA_POINTER, ICA_MAX_INSNS, CTX_ICA);
    if (!st.interrupt && ((dcr >> 4) & 1)) {                   /* DCAs on */
        static scc_frame_t frame;                              /* large: not on the stack */
        scc66470_render(v, dram, dram_size, &frame);
        st.interrupt = frame.interrupt;
    }
    if (st.interrupt && !(v->csr_control & 0x8000u)) {
        v->it1 = 1;
        if (!v->it_logged && video_log_enabled()) {
            fprintf(stderr, "[VIDEO] ICA %05X: INTERRUPT -> IT1 (IRQ level 5)\n",
                    (unsigned)(v->ica_pointer ? v->ica_pointer : ICA_POINTER));
            v->it_logged = 1;
        }
    }
}

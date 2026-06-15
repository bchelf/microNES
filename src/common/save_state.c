#include "save_state.h"

#include <stdio.h>
#include <string.h>

uint32_t save_state_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t ptr_to_offset(const uint8_t *base, const uint8_t *ptr) {
    if (ptr == NULL) {
        return SAVE_STATE_PTR_NONE;
    }
    return (uint32_t)(ptr - base);
}

static const uint8_t *offset_to_ptr(const uint8_t *base, uint32_t offset) {
    if (offset == SAVE_STATE_PTR_NONE) {
        return NULL;
    }
    return base + offset;
}

void save_state_capture(const Nes *nes, uint32_t rom_checksum, uint32_t rom_image_size,
                        uint32_t elapsed_seconds, SaveStateBlob *out) {
    memset(out, 0, sizeof(*out));

    out->header.magic          = SAVE_STATE_MAGIC;
    out->header.version        = SAVE_STATE_VERSION;
    out->header.rom_checksum    = rom_checksum;
    out->header.rom_image_size  = rom_image_size;
    out->header.elapsed_seconds = elapsed_seconds;

    /* CPU */
    const Cpu6502 *cpu = &nes->cpu;
    out->cpu.a           = cpu->a;
    out->cpu.x           = cpu->x;
    out->cpu.y           = cpu->y;
    out->cpu.sp          = cpu->sp;
    out->cpu.p           = cpu->p;
    out->cpu.pc          = cpu->pc;
    out->cpu.cycles      = cpu->cycles;
    out->cpu.last_opcode = cpu->last_opcode;
    out->cpu.jammed      = cpu->jammed ? 1u : 0u;
    out->cpu.insn_count  = cpu->insn_count;

    /* RAM */
    memcpy(out->cpu_ram, nes->cpu_ram, sizeof(out->cpu_ram));
    memcpy(out->wram, nes->wram, sizeof(out->wram));

    /* Controllers */
    for (int i = 0; i < 2; ++i) {
        out->controllers[i].shift_register = nes->controllers[i].shift_register;
        out->controllers[i].strobe         = nes->controllers[i].strobe ? 1u : 0u;
        out->controllers[i].live_buttons   = nes->controllers[i].live_state.buttons;
    }

    /* PPU */
    const Ppu *ppu = &nes->ppu;
    out->ppu.scanline                  = (int32_t)ppu->scanline;
    out->ppu.cycle                     = (int32_t)ppu->cycle;
    out->ppu.cycles_remaining          = ppu->cycles_remaining;
    out->ppu.frame_ready               = ppu->frame_ready ? 1u : 0u;
    out->ppu.scanline_ready            = ppu->scanline_ready ? 1u : 0u;
    out->ppu.nmi_pending               = ppu->nmi_pending ? 1u : 0u;
    out->ppu.completed_frame_ready     = ppu->completed_frame_ready ? 1u : 0u;
    out->ppu.ctrl                      = ppu->ctrl;
    out->ppu.mask                      = ppu->mask;
    out->ppu.status                    = ppu->status;
    out->ppu.oam_addr                  = ppu->oam_addr;
    out->ppu.read_buffer               = ppu->read_buffer;
    out->ppu.fine_x                    = ppu->fine_x;
    out->ppu.write_toggle              = ppu->write_toggle ? 1u : 0u;
    out->ppu.scroll_x                  = ppu->scroll_x;
    out->ppu.scroll_y                  = ppu->scroll_y;
    out->ppu.render_fine_x             = ppu->render_fine_x;
    out->ppu.render_scroll_x           = ppu->render_scroll_x;
    out->ppu.render_scroll_y           = ppu->render_scroll_y;
    out->ppu.render_base_nametable     = ppu->render_base_nametable;
    out->ppu.sprite0_hit_ever          = ppu->sprite0_hit_ever ? 1u : 0u;
    out->ppu.max_scanline_sprite_count = ppu->max_scanline_sprite_count;
    out->ppu.vram_addr                 = ppu->vram_addr;
    out->ppu.temp_addr                 = ppu->temp_addr;
    out->ppu.render_vram_addr          = ppu->render_vram_addr;
    out->ppu.frame_count                = ppu->frame_count;
    out->ppu.completed_frame_count      = ppu->completed_frame_count;
    memcpy(out->ppu.oam, ppu->oam, sizeof(out->ppu.oam));
    memcpy(out->ppu.nametables, ppu->nametables, sizeof(out->ppu.nametables));
    memcpy(out->ppu.palette, ppu->palette, sizeof(out->ppu.palette));

    /* APU */
    const Apu *apu = &nes->apu;
    memcpy(out->apu.registers, apu->registers, sizeof(out->apu.registers));
    out->apu.cpu_cycles           = apu->cpu_cycles;
    out->apu.frame_counter_cycle  = apu->frame_counter_cycle;
    out->apu.frame_counter_steps  = apu->frame_counter_steps;
    out->apu.sample_phase         = apu->sample_phase;
    out->apu.status               = apu->status;
    out->apu.frame_counter_mode_5 = apu->frame_counter_mode_5 ? 1u : 0u;
    out->apu.frame_irq_inhibit    = apu->frame_irq_inhibit ? 1u : 0u;
    out->apu.hp_prev_x            = apu->hp_prev_x;
    out->apu.hp_prev_y            = apu->hp_prev_y;
    out->apu.lp_prev_y            = apu->lp_prev_y;

    for (int i = 0; i < 2; ++i) {
        const ApuPulseChannel *p = &apu->pulse[i];
        SaveStatePulse *sp = &out->apu.pulse[i];
        sp->enabled               = p->enabled ? 1u : 0u;
        sp->length_halt           = p->length_halt ? 1u : 0u;
        sp->constant_volume       = p->constant_volume ? 1u : 0u;
        sp->envelope_start        = p->envelope_start ? 1u : 0u;
        sp->sweep_enabled         = p->sweep_enabled ? 1u : 0u;
        sp->sweep_negate          = p->sweep_negate ? 1u : 0u;
        sp->sweep_reload          = p->sweep_reload ? 1u : 0u;
        sp->sweep_ones_complement = p->sweep_ones_complement ? 1u : 0u;
        sp->duty                  = p->duty;
        sp->duty_step             = p->duty_step;
        sp->volume_period         = p->volume_period;
        sp->envelope_divider      = p->envelope_divider;
        sp->envelope_decay        = p->envelope_decay;
        sp->sweep_period          = p->sweep_period;
        sp->sweep_divider         = p->sweep_divider;
        sp->sweep_shift           = p->sweep_shift;
        sp->length_counter        = p->length_counter;
        sp->timer_period          = p->timer_period;
        sp->timer_counter         = p->timer_counter;
    }

    {
        const ApuTriangleChannel *t = &apu->triangle;
        SaveStateTriangle *st = &out->apu.triangle;
        st->enabled            = t->enabled ? 1u : 0u;
        st->control_flag       = t->control_flag ? 1u : 0u;
        st->linear_reload_flag = t->linear_reload_flag ? 1u : 0u;
        st->sequence_step      = t->sequence_step;
        st->linear_reload_value = t->linear_reload_value;
        st->linear_counter     = t->linear_counter;
        st->length_counter     = t->length_counter;
        st->timer_period       = t->timer_period;
        st->timer_counter      = t->timer_counter;
    }

    {
        const ApuNoiseChannel *n = &apu->noise;
        SaveStateNoise *sn = &out->apu.noise;
        sn->enabled          = n->enabled ? 1u : 0u;
        sn->length_halt      = n->length_halt ? 1u : 0u;
        sn->constant_volume  = n->constant_volume ? 1u : 0u;
        sn->envelope_start   = n->envelope_start ? 1u : 0u;
        sn->mode             = n->mode ? 1u : 0u;
        sn->volume_period    = n->volume_period;
        sn->envelope_divider = n->envelope_divider;
        sn->envelope_decay   = n->envelope_decay;
        sn->length_counter   = n->length_counter;
        sn->period_index     = n->period_index;
        sn->timer_period     = n->timer_period;
        sn->timer_counter    = n->timer_counter;
        sn->shift_register   = n->shift_register;
    }

    {
        const ApuDmcChannel *d = &apu->dmc;
        SaveStateDmc *sd = &out->apu.dmc;
        sd->enabled              = d->enabled ? 1u : 0u;
        sd->irq_enabled          = d->irq_enabled ? 1u : 0u;
        sd->loop_flag            = d->loop_flag ? 1u : 0u;
        sd->silence_flag         = d->silence_flag ? 1u : 0u;
        sd->sample_buffer_filled = d->sample_buffer_filled ? 1u : 0u;
        sd->rate_index           = d->rate_index;
        sd->output_level         = d->output_level;
        sd->sample_buffer        = d->sample_buffer;
        sd->shift_register       = d->shift_register;
        sd->bits_remaining       = d->bits_remaining;
        sd->timer_period         = d->timer_period;
        sd->timer_counter        = d->timer_counter;
        sd->sample_address       = d->sample_address;
        sd->sample_length        = d->sample_length;
        sd->current_address      = d->current_address;
        sd->bytes_remaining      = d->bytes_remaining;
    }

    /* Cartridge */
    const NesCartridge *cart = &nes->cartridge;
    SaveStateCartridge *sc = &out->cartridge;
    sc->mirror_mode  = (uint32_t)cart->mirror_mode;
    sc->mmc1_shift       = cart->mmc1_shift;
    sc->mmc1_shift_count = cart->mmc1_shift_count;
    sc->mmc1_control     = cart->mmc1_control;
    sc->mmc1_chr0        = cart->mmc1_chr0;
    sc->mmc1_chr1        = cart->mmc1_chr1;
    sc->mmc1_prg         = cart->mmc1_prg;
    sc->mmc3_bank_select = cart->mmc3_bank_select;
    memcpy(sc->mmc3_bank_data, cart->mmc3_bank_data, sizeof(sc->mmc3_bank_data));
    sc->mmc3_irq_latch   = cart->mmc3_irq_latch;
    sc->mmc3_irq_counter = cart->mmc3_irq_counter;
    sc->mmc3_irq_reload  = cart->mmc3_irq_reload ? 1u : 0u;
    sc->mmc3_irq_enabled = cart->mmc3_irq_enabled ? 1u : 0u;
    sc->irq_pending      = cart->irq_pending ? 1u : 0u;
    sc->cnrom_chr_bank   = cart->cnrom_chr_bank;
    sc->mmc2_chr0_fd = cart->mmc2_chr0_fd;
    sc->mmc2_chr0_fe = cart->mmc2_chr0_fe;
    sc->mmc2_chr1_fd = cart->mmc2_chr1_fd;
    sc->mmc2_chr1_fe = cart->mmc2_chr1_fe;
    sc->mmc2_latch0  = cart->mmc2_latch0 ? 1u : 0u;
    sc->mmc2_latch1  = cart->mmc2_latch1 ? 1u : 0u;
    sc->m40_irq_counter = cart->m40_irq_counter;
    sc->m40_irq_enabled = cart->m40_irq_enabled ? 1u : 0u;

    sc->prg_bank_lo_offset = ptr_to_offset(cart->prg_rom, cart->prg_bank_lo);
    sc->prg_bank_hi_offset = ptr_to_offset(cart->prg_rom, cart->prg_bank_hi);
    for (int i = 0; i < 4; ++i) {
        sc->prg_banks_8k_offset[i] = ptr_to_offset(cart->prg_rom, cart->prg_banks_8k[i]);
    }
    sc->m40_prg_6000_offset = ptr_to_offset(cart->prg_rom, cart->m40_prg_6000);

    if (cart->chr_is_ram && cart->chr_data != NULL) {
        size_t n = cart->chr_size;
        if (n > sizeof(sc->chr_ram)) {
            n = sizeof(sc->chr_ram);
        }
        sc->chr_ram_size = (uint32_t)n;
        memcpy(sc->chr_ram, cart->chr_data, n);
    }

    out->crc32 = save_state_crc32((const uint8_t *)out,
                                   offsetof(SaveStateBlob, crc32));
}

bool save_state_apply(Nes *nes, const SaveStateBlob *blob,
                      uint32_t rom_checksum, uint32_t rom_image_size) {
    if (nes == NULL || blob == NULL) {
        return false;
    }
    if (blob->header.magic != SAVE_STATE_MAGIC ||
        blob->header.version != SAVE_STATE_VERSION) {
        printf("[save_state] apply: bad header (magic=0x%08x version=%u, "
               "expected magic=0x%08x version=%u)\n",
               (unsigned)blob->header.magic, (unsigned)blob->header.version,
               (unsigned)SAVE_STATE_MAGIC, (unsigned)SAVE_STATE_VERSION);
        return false;
    }
    uint32_t crc = save_state_crc32((const uint8_t *)blob,
                                     offsetof(SaveStateBlob, crc32));
    if (crc != blob->crc32) {
        printf("[save_state] apply: CRC mismatch (computed=0x%08x stored=0x%08x)\n",
               (unsigned)crc, (unsigned)blob->crc32);
        return false;
    }
    if (blob->header.rom_checksum != rom_checksum ||
        blob->header.rom_image_size != rom_image_size) {
        printf("[save_state] apply: ROM mismatch (blob checksum=0x%08x size=%u, "
               "current checksum=0x%08x size=%u)\n",
               (unsigned)blob->header.rom_checksum, (unsigned)blob->header.rom_image_size,
               (unsigned)rom_checksum, (unsigned)rom_image_size);
        return false;
    }

    /* CPU */
    Cpu6502 *cpu = &nes->cpu;
    cpu->a           = blob->cpu.a;
    cpu->x           = blob->cpu.x;
    cpu->y           = blob->cpu.y;
    cpu->sp          = blob->cpu.sp;
    cpu->p           = blob->cpu.p;
    cpu->pc          = blob->cpu.pc;
    cpu->cycles      = blob->cpu.cycles;
    cpu->last_opcode = blob->cpu.last_opcode;
    cpu->jammed      = blob->cpu.jammed != 0;
    cpu->insn_count  = blob->cpu.insn_count;

    /* RAM */
    memcpy(nes->cpu_ram, blob->cpu_ram, sizeof(nes->cpu_ram));
    memcpy(nes->wram, blob->wram, sizeof(nes->wram));

    /* Controllers */
    for (int i = 0; i < 2; ++i) {
        nes->controllers[i].shift_register   = blob->controllers[i].shift_register;
        nes->controllers[i].strobe           = blob->controllers[i].strobe != 0;
        nes->controllers[i].live_state.buttons = blob->controllers[i].live_buttons;
    }

    /* PPU */
    Ppu *ppu = &nes->ppu;
    ppu->scanline                  = (int)blob->ppu.scanline;
    ppu->cycle                     = (int)blob->ppu.cycle;
    ppu->cycles_remaining          = blob->ppu.cycles_remaining;
    ppu->frame_ready               = blob->ppu.frame_ready != 0;
    ppu->scanline_ready            = blob->ppu.scanline_ready != 0;
    ppu->nmi_pending                = blob->ppu.nmi_pending != 0;
    ppu->completed_frame_ready     = blob->ppu.completed_frame_ready != 0;
    ppu->ctrl                      = blob->ppu.ctrl;
    ppu->mask                       = blob->ppu.mask;
    ppu->status                     = blob->ppu.status;
    ppu->oam_addr                   = blob->ppu.oam_addr;
    ppu->read_buffer                = blob->ppu.read_buffer;
    ppu->fine_x                     = blob->ppu.fine_x;
    ppu->write_toggle               = blob->ppu.write_toggle != 0;
    ppu->scroll_x                   = blob->ppu.scroll_x;
    ppu->scroll_y                   = blob->ppu.scroll_y;
    ppu->render_fine_x              = blob->ppu.render_fine_x;
    ppu->render_scroll_x            = blob->ppu.render_scroll_x;
    ppu->render_scroll_y            = blob->ppu.render_scroll_y;
    ppu->render_base_nametable      = blob->ppu.render_base_nametable;
    ppu->sprite0_hit_ever            = blob->ppu.sprite0_hit_ever != 0;
    ppu->max_scanline_sprite_count   = blob->ppu.max_scanline_sprite_count;
    ppu->vram_addr                  = blob->ppu.vram_addr;
    ppu->temp_addr                  = blob->ppu.temp_addr;
    ppu->render_vram_addr           = blob->ppu.render_vram_addr;
    ppu->frame_count                 = blob->ppu.frame_count;
    ppu->completed_frame_count       = blob->ppu.completed_frame_count;
    memcpy(ppu->oam, blob->ppu.oam, sizeof(ppu->oam));
    memcpy(ppu->nametables, blob->ppu.nametables, sizeof(ppu->nametables));
    memcpy(ppu->palette, blob->ppu.palette, sizeof(ppu->palette));

    /* APU.  dmc_bus_read/dmc_bus_read_user and the PCM ring buffer are left
     * as set up by apu_init/apu_reset during the relaunch that precedes this
     * call. */
    Apu *apu = &nes->apu;
    memcpy(apu->registers, blob->apu.registers, sizeof(apu->registers));
    apu->cpu_cycles           = blob->apu.cpu_cycles;
    apu->frame_counter_cycle  = blob->apu.frame_counter_cycle;
    apu->frame_counter_steps  = blob->apu.frame_counter_steps;
    apu->sample_phase         = blob->apu.sample_phase;
    apu->status               = blob->apu.status;
    apu->frame_counter_mode_5 = blob->apu.frame_counter_mode_5 != 0;
    apu->frame_irq_inhibit    = blob->apu.frame_irq_inhibit != 0;
    apu->hp_prev_x            = blob->apu.hp_prev_x;
    apu->hp_prev_y            = blob->apu.hp_prev_y;
    apu->lp_prev_y            = blob->apu.lp_prev_y;

    for (int i = 0; i < 2; ++i) {
        ApuPulseChannel *p = &apu->pulse[i];
        const SaveStatePulse *sp = &blob->apu.pulse[i];
        p->enabled               = sp->enabled != 0;
        p->length_halt           = sp->length_halt != 0;
        p->constant_volume       = sp->constant_volume != 0;
        p->envelope_start        = sp->envelope_start != 0;
        p->sweep_enabled         = sp->sweep_enabled != 0;
        p->sweep_negate          = sp->sweep_negate != 0;
        p->sweep_reload          = sp->sweep_reload != 0;
        p->sweep_ones_complement = sp->sweep_ones_complement != 0;
        p->duty                  = sp->duty;
        p->duty_step             = sp->duty_step;
        p->volume_period         = sp->volume_period;
        p->envelope_divider      = sp->envelope_divider;
        p->envelope_decay        = sp->envelope_decay;
        p->sweep_period          = sp->sweep_period;
        p->sweep_divider         = sp->sweep_divider;
        p->sweep_shift           = sp->sweep_shift;
        p->length_counter        = sp->length_counter;
        p->timer_period          = sp->timer_period;
        p->timer_counter         = sp->timer_counter;
    }

    {
        ApuTriangleChannel *t = &apu->triangle;
        const SaveStateTriangle *st = &blob->apu.triangle;
        t->enabled            = st->enabled != 0;
        t->control_flag       = st->control_flag != 0;
        t->linear_reload_flag = st->linear_reload_flag != 0;
        t->sequence_step      = st->sequence_step;
        t->linear_reload_value = st->linear_reload_value;
        t->linear_counter     = st->linear_counter;
        t->length_counter     = st->length_counter;
        t->timer_period       = st->timer_period;
        t->timer_counter      = st->timer_counter;
    }

    {
        ApuNoiseChannel *n = &apu->noise;
        const SaveStateNoise *sn = &blob->apu.noise;
        n->enabled          = sn->enabled != 0;
        n->length_halt      = sn->length_halt != 0;
        n->constant_volume  = sn->constant_volume != 0;
        n->envelope_start   = sn->envelope_start != 0;
        n->mode             = sn->mode != 0;
        n->volume_period    = sn->volume_period;
        n->envelope_divider = sn->envelope_divider;
        n->envelope_decay   = sn->envelope_decay;
        n->length_counter   = sn->length_counter;
        n->period_index     = sn->period_index;
        n->timer_period     = sn->timer_period;
        n->timer_counter    = sn->timer_counter;
        n->shift_register   = sn->shift_register;
    }

    {
        ApuDmcChannel *d = &apu->dmc;
        const SaveStateDmc *sd = &blob->apu.dmc;
        d->enabled              = sd->enabled != 0;
        d->irq_enabled          = sd->irq_enabled != 0;
        d->loop_flag            = sd->loop_flag != 0;
        d->silence_flag         = sd->silence_flag != 0;
        d->sample_buffer_filled = sd->sample_buffer_filled != 0;
        d->rate_index           = sd->rate_index;
        d->output_level         = sd->output_level;
        d->sample_buffer        = sd->sample_buffer;
        d->shift_register       = sd->shift_register;
        d->bits_remaining       = sd->bits_remaining;
        d->timer_period         = sd->timer_period;
        d->timer_counter        = sd->timer_counter;
        d->sample_address       = sd->sample_address;
        d->sample_length        = sd->sample_length;
        d->current_address      = sd->current_address;
        d->bytes_remaining      = sd->bytes_remaining;
    }

    /* Cartridge */
    NesCartridge *cart = &nes->cartridge;
    const SaveStateCartridge *sc = &blob->cartridge;
    cart->mirror_mode = (NesMirrorMode)sc->mirror_mode;
    cart->mmc1_shift       = sc->mmc1_shift;
    cart->mmc1_shift_count = sc->mmc1_shift_count;
    cart->mmc1_control     = sc->mmc1_control;
    cart->mmc1_chr0        = sc->mmc1_chr0;
    cart->mmc1_chr1        = sc->mmc1_chr1;
    cart->mmc1_prg         = sc->mmc1_prg;
    cart->mmc3_bank_select = sc->mmc3_bank_select;
    memcpy(cart->mmc3_bank_data, sc->mmc3_bank_data, sizeof(cart->mmc3_bank_data));
    cart->mmc3_irq_latch   = sc->mmc3_irq_latch;
    cart->mmc3_irq_counter = sc->mmc3_irq_counter;
    cart->mmc3_irq_reload  = sc->mmc3_irq_reload != 0;
    cart->mmc3_irq_enabled = sc->mmc3_irq_enabled != 0;
    cart->irq_pending      = sc->irq_pending != 0;
    cart->cnrom_chr_bank   = sc->cnrom_chr_bank;
    cart->mmc2_chr0_fd = sc->mmc2_chr0_fd;
    cart->mmc2_chr0_fe = sc->mmc2_chr0_fe;
    cart->mmc2_chr1_fd = sc->mmc2_chr1_fd;
    cart->mmc2_chr1_fe = sc->mmc2_chr1_fe;
    cart->mmc2_latch0  = sc->mmc2_latch0 != 0;
    cart->mmc2_latch1  = sc->mmc2_latch1 != 0;
    cart->m40_irq_counter = sc->m40_irq_counter;
    cart->m40_irq_enabled = sc->m40_irq_enabled != 0;

    cart->prg_bank_lo = offset_to_ptr(cart->prg_rom, sc->prg_bank_lo_offset);
    cart->prg_bank_hi = offset_to_ptr(cart->prg_rom, sc->prg_bank_hi_offset);
    for (int i = 0; i < 4; ++i) {
        cart->prg_banks_8k[i] = offset_to_ptr(cart->prg_rom, sc->prg_banks_8k_offset[i]);
    }
    cart->m40_prg_6000 = offset_to_ptr(cart->prg_rom, sc->m40_prg_6000_offset);
    nes_sync_prg_cache(nes);

    if (cart->chr_is_ram && cart->chr_data != NULL && sc->chr_ram_size > 0) {
        size_t n = sc->chr_ram_size;
        if (n > cart->chr_size) {
            n = cart->chr_size;
        }
        if (n > sizeof(sc->chr_ram)) {
            n = sizeof(sc->chr_ram);
        }
        memcpy(cart->chr_data, sc->chr_ram, n);
        cart_rebuild_chr_row_cache(cart);
    }

    return true;
}

void save_state_format_elapsed(uint32_t elapsed_seconds, char *out, size_t out_size) {
    unsigned minutes = elapsed_seconds / 60u;
    unsigned seconds = elapsed_seconds % 60u;
    snprintf(out, out_size, "%02u:%02u", minutes, seconds);
}

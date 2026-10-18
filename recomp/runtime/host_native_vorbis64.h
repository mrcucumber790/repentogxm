#ifndef ISAAC_HOST_NATIVE_VORBIS64_H
#define ISAAC_HOST_NATIVE_VORBIS64_H

/* Native stb_vorbis for the KAGE OGG path on 64-bit hosts (linux64, Switch;
 * ISAAC_NATIVE_VORBIS in recomp/linux64/CMakeLists.txt).
 *
 * WHY NOT THE VITA MODULE.  host_vita_native_vorbis.c clones the guest's
 * decoder state into host memory and relocates its internal pointers; that
 * needs host pointers that are 32 bits wide (guest address == host address).
 * On a 64-bit host the guest lives in a 4 GiB window and stb_vorbis's
 * structs have a different layout natively, so this module never clones.
 *
 * MODEL.  Every guest state the game opens through the two cross-unit
 * entries StreamSourceOgg uses gets a host-side native `stb_vorbis *`
 * (the "mirror"), opened by the native decoder (the same vendored v1.04 the
 * PE links, recomp/runtime/third_party/stb_vorbis) over the same bytes,
 * right after the translated open succeeded.  The mirror is keyed by the
 * guest state address and tagged by a marker written into the guest state's
 * setup_temp_memory_required word (+0x10, only meaningful during setup), so
 * a different state that later appears at the same address is never served
 * by a stale mirror.  The guest state itself is left exactly as the
 * translated open produced it (positioned after the first frame).
 *
 *   memory (whole-sample SFX loads, sub_005a36a0): stb_vorbis_open_memory
 *     (sub_005bd8b0; data = ecx, len = edx, [esp+4] int *error, [esp+8]
 *     stb_vorbis_alloc *) -> native stb_vorbis_open_memory over guest_ptr(data).
 *   stream (music, StreamSourceOgg::Open): stb_vorbis_open_file (sub_005bd740;
 *     KAGE stream = ecx, [esp+4] error, [esp+8] alloc) -> native
 *     stb_vorbis_open_file_section at the guest state's own f_start (+0x18)
 *     with its stream_len (+0x2c).  The native decoder's fgetc/fread/fseek/
 *     ftell go to a per-mirror host block cache that is filled through the
 *     guest stream object (vtable +0x08 tell, +0x0c seek, +0x14 read) with
 *     guest_call on the calling thread, via one guest-heap staging block.
 *     Every refill asks the stream where it is (tell) and seeks only when
 *     something else moved it, so guest code touching the stream between
 *     calls is harmless.
 *
 * SERVING.  stb_vorbis_get_samples_short_interleaved (sub_005bdf80; f = ecx,
 * channels = edx, [esp+4] short *buffer, [esp+8] num_shorts, caller cleans,
 * eax = frames, plain `ret`) on a mirrored state decodes natively straight
 * into the guest buffer and returns like the translated epilogue.  The
 * inlined seek_start in StreamSourceOgg::Decode (set_file_offset 0x005b7740
 * called from 0x5a2192, vorbis_pump_first_frame 0x005bb260 called from
 * 0x5a21b4; both plain `ret`, results unused) becomes a native
 * stb_vorbis_seek_start and two no-ops, recognised by their return
 * addresses; every other caller of those two runs the translated body.
 * vorbis_deinit (sub_005bd310) drops the mirror, then runs the real body.
 * A StreamSourceOgg closes by freeing the stb_vorbis_alloc buffer without
 * deinit, so the guest heap's free observer (isaac_nv_guest_buffer_freed,
 * weak in host_vita_heap.c) flags the mirror and the next wrapped call
 * reclaims it.  The flagging takes only a short spinlock and never blocks on
 * the decode lock.
 *
 * FALLBACKS (translated body, guest state authoritative):
 *   - native open fails / layout mismatch / pool full: no mirror;
 *   - a native decode that breaks (decoder assertion -> longjmp, guest
 *     stream I/O failure) before the mirror delivered any frame since its
 *     last sync point (open or native seek_start): the guest stream is put
 *     back where the translated open left it, the mirror is dropped and the
 *     translated body serves the call -- the guest state never moved;
 *   - a break after frames were delivered: that call returns what it has
 *     (possibly 0, an end of stream for the guest), the mirror is marked
 *     broken, and the next seek_start drops it and runs the translated
 *     set_file_offset + pump, which resynchronise the guest state from the
 *     stream start (music loops); a whole sample just ends early.
 *
 * THREADS.  All wrapped work is serialised by one recursive mutex and runs
 * on the calling (game) thread; no worker.
 *
 * VERIFY.  ISAAC_NATIVE_VORBIS_VERIFY=<n> in the environment (or the same
 * compile definition as default) verifies the first n mirrors: each wrapped
 * call decodes natively into host scratch, puts the guest stream back, runs
 * the translated body on the real guest state, compares frame counts and
 * int16 samples and returns the TRANSLATED result, so the guest stays
 * authoritative.  Totals appear in the receipt.  Measured on linux64
 * (2026-10-17): frame counts always equal, samples differ by at most 1 LSB
 * in ~0.1-0.2% of int16 values (the native decoder computes its own sin/cos
 * twiddles and windows instead of cloning the translated tables).
 *
 * Other debug environment knobs (all off when unset, as on Switch):
 *   ISAAC_NATIVE_VORBIS=0                 no mirrors (A/B runs);
 *   ISAAC_NATIVE_VORBIS_SELFTEST=<n>      for the first n music streams, read
 *       the whole OGG into a guest buffer and run it through the wrapped
 *       whole-sample entries (open_memory/get_samples/deinit via guest_call),
 *       verified and then native, comparing PCM CRCs;
 *   ISAAC_NATIVE_VORBIS_TEST_LOOP_FRAMES=<n>  a stream mirror reports end of
 *       stream after n frames so the native seek_start path runs.
 *
 * RECEIPT.  One `KAGE NATIVE VORBIS:` line at shutdown (entry_vita.c), plus
 * FALLBACK lines on power-of-two counts only. */

void isaac_native_vorbis_report(const char *why);
/* Decode active music streams ahead; once per main loop iteration on the
 * game thread (weak-referenced by the SDL backend). */
void isaac_native_vorbis_tick(void);

#endif

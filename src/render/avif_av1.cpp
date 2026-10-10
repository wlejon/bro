#include "render/avif_av1.h"

#include "broimage/heif.h"

#include <dav1d/dav1d.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <thread>

namespace bro::render {

namespace {

void noFree(const uint8_t*, void*) {}

// One AV1 still: a fresh decoder per image (a grid's tiles each make one).
// Its threads are capped: a phone photo's 48 tiles each spinning up every
// core would cost more than it saves.
bool decodeAv1(const uint8_t* obus, std::size_t size, broimage::YuvSink sink, void* ctx, std::string* why) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = int(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
    s.max_frame_delay = 1;
    s.all_layers = 0;                    // the highest spatial layer only (AVIF: the image)
    s.frame_size_limit = 1u << 28;       // as broimage's canvas limit
    Dav1dContext* c = nullptr;
    if (dav1d_open(&c, &s) < 0 || !c) {
        if (why) *why = "dav1d could not start";
        return false;
    }
    Dav1dData data{};
    if (dav1d_data_wrap(&data, obus, size, noFree, nullptr) < 0) {
        dav1d_close(&c);
        if (why) *why = "dav1d could not take the data";
        return false;
    }
    Dav1dPicture pic{};
    int res = 0;
    bool got = false;
    // Feed it, taking pictures as they come; once everything is sent,
    // get_picture drains what is in flight. A still is one temporal unit.
    for (int drains = 0; drains < 16;) {
        if (data.sz > 0) {
            res = dav1d_send_data(c, &data);
            if (res < 0 && res != DAV1D_ERR(EAGAIN)) break;
        }
        res = dav1d_get_picture(c, &pic);
        if (res == 0) {
            got = true;
            break;
        }
        if (res != DAV1D_ERR(EAGAIN)) break;
        if (data.sz == 0) ++drains;
    }
    if (data.sz > 0) dav1d_data_unref(&data);
    bool ok = false;
    if (got) {
        broimage::YuvPlanes f;
        f.width = pic.p.w;
        f.height = pic.p.h;
        f.bit_depth = pic.p.bpc;
        switch (pic.p.layout) {
            case DAV1D_PIXEL_LAYOUT_I400: f.monochrome = true; f.chroma_shift_x = f.chroma_shift_y = 1; break;
            case DAV1D_PIXEL_LAYOUT_I420: f.chroma_shift_x = 1; f.chroma_shift_y = 1; break;
            case DAV1D_PIXEL_LAYOUT_I422: f.chroma_shift_x = 1; f.chroma_shift_y = 0; break;
            case DAV1D_PIXEL_LAYOUT_I444: f.chroma_shift_x = 0; f.chroma_shift_y = 0; break;
        }
        for (int i = 0; i < 3; ++i) f.planes[i] = pic.data[i];
        f.strides[0] = pic.stride[0];
        f.strides[1] = f.strides[2] = pic.stride[1];
        if (pic.seq_hdr) {
            f.color_primaries = int(pic.seq_hdr->pri);
            f.transfer = int(pic.seq_hdr->trc);
            f.matrix = int(pic.seq_hdr->mtrx);
            f.full_range = pic.seq_hdr->color_range != 0;
            f.chroma_position = int(pic.seq_hdr->chr);
        }
        ok = sink(ctx, f);
        if (!ok && why && why->empty()) *why = "the frame was refused";
        dav1d_picture_unref(&pic);
    } else if (why) {
        *why = "dav1d decoded no picture (error " + std::to_string(res) + ")";
    }
    dav1d_close(&c);
    return ok;
}

}  // namespace

void registerAvifDecoder() { broimage::set_av1_decoder(decodeAv1); }

}  // namespace bro::render

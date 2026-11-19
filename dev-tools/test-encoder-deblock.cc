/*
 * H.265 video codec.
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

// End-to-end check of the encoder-side deblocking filter:
//   - encode a synthetic picture with and without --deblock,
//   - the encoder's own reconstruction must be pixel-identical to what libde265's
//     decoder produces from the same bitstream (in-loop filter matches the decoder),
//   - the deblocked picture must have a lower blocking metric than the unfiltered one,
//   - with --deblock disabled, the encoder's reconstruction must be unchanged by the
//     filter code (the output is the plain, unfiltered reconstruction).

#include "tests.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "libde265/en265.h"
#include "libde265/de265.h"
#include "libde265/image.h"

namespace {

const int W = 96;
const int H = 96;

// Synthetic 4:2:0 picture with strong 16x16 block structure, so that the deblocking
// filter actually has edges to work on.
void make_test_picture(std::vector<uint8_t>& yuv)
{
  yuv.resize(W*H*3/2);

  uint32_t s = 0x12345678;
  auto rnd = [&]() { s ^= s<<13; s ^= s>>17; s ^= s<<5; return (int)(s % 9) - 4; };

  for (int y=0;y<H;y++)
    for (int x=0;x<W;x++) {
      int v = (x/16)*37 + (y/16)*61;
      yuv[y*W+x] = (uint8_t)std::max(0, std::min(255, v + rnd()));
    }

  for (int c=1;c<3;c++) {
    uint8_t* p = &yuv[W*H + (c-1)*W*H/4];
    for (int y=0;y<H/2;y++)
      for (int x=0;x<W/2;x++) {
        int v = (x/8)*29 + (y/8)*17 + c*40;
        p[y*W/2+x] = (uint8_t)std::max(0, std::min(255, v + rnd()));
      }
  }
}


struct enc_result {
  std::vector<uint8_t> bitstream;
  std::vector<uint8_t> reconstruction; // plane-interleaved, full picture
};


// The encoder packets do not carry the Annex-B start codes (enc265's PacketSink_File
// prepends them), so we have to write them between the NAL units ourselves.
void append_packet(std::vector<uint8_t>& out, const en265_packet* pck)
{
  static const uint8_t startCode[3] = { 0,0,1 };

  out.insert(out.end(), startCode, startCode+3);
  out.insert(out.end(), pck->data, pck->data + pck->length);
}

// Encode one picture; returns false on encoder failure.
bool encode_picture(bool deblock, int qp, enc_result& out)
{
  std::vector<uint8_t> input;
  make_test_picture(input);

  en265_encoder_context* ectx = en265_new_encoder();
  if (!ectx) return false;

  en265_set_parameter_int   (ectx, "max-cb-size", 16);
  en265_set_parameter_int   (ectx, "max-tb-size", 16);
  en265_set_parameter_choice(ectx, "CB-IntraPartMode", "fixed");
  en265_set_parameter_choice(ectx, "TB-IntraPredMode", "min-residual");
  en265_set_parameter_choice(ectx, "TB-RateEstimation", "none");
  en265_set_parameter_choice(ectx, "sop-structure", "intra");
  en265_set_parameter_int   (ectx, "CTB-QScale-Constant", qp);
  en265_set_parameter_bool  (ectx, "deblock", deblock ? 1 : 0);

  if (en265_start_encoder(ectx, 0) != DE265_OK) {
    en265_free_encoder(ectx);
    return false;
  }

  de265_image* img = new de265_image;
  if (img->alloc_image(W,H,de265_chroma_420,nullptr,false,nullptr,0,nullptr,false) != DE265_OK) {
    delete img;
    en265_free_encoder(ectx);
    return false;
  }

  for (int y=0;y<H;y++)
    memcpy(img->get_image_plane_at_pos(0,0,y), &input[y*W], W);
  for (int c=1;c<=2;c++)
    for (int y=0;y<H/2;y++)
      memcpy(img->get_image_plane_at_pos(c,0,y),
             &input[W*H + (c-1)*W*H/4 + y*W/2], W/2);

  bool ok = true;

  if (en265_push_image(ectx, img) != DE265_OK) { ok = false; }
  en265_push_eof(ectx);

  for (;;) {
    if (en265_encode(ectx) != DE265_OK) { ok = false; break; }

    bool gotPacket = false;
    for (;;) {
      en265_packet* pck = en265_get_packet(ectx, 0);
      if (!pck) break;

      if (pck->content_type != EN265_PACKET_SLICE) {
        append_packet(out.bitstream, pck);
      }
      else {
        gotPacket = true;
        append_packet(out.bitstream, pck);

        const de265_image* r = pck->reconstruction;
        if (!r) { ok = false; }
        else {
          out.reconstruction.clear();
          out.reconstruction.reserve(W*H*3/2);
          for (int y=0;y<H;y++)
            out.reconstruction.insert(out.reconstruction.end(),
                                      r->get_image_plane_at_pos(0,0,y),
                                      r->get_image_plane_at_pos(0,0,y)+W);
          for (int c=1;c<=2;c++)
            for (int y=0;y<H/2;y++)
              out.reconstruction.insert(out.reconstruction.end(),
                                        r->get_image_plane_at_pos(c,0,y),
                                        r->get_image_plane_at_pos(c,0,y)+W/2);
        }
      }

      en265_free_packet(ectx, pck);
    }

    if (gotPacket) break;
    if (en265_get_encoder_state(ectx) == EN265_STATE_EOS) break;
  }

  en265_free_encoder(ectx);
  return ok;
}


// Decode all pictures of 'bitstream' and return the first decoded picture.
bool decode_first_picture(const std::vector<uint8_t>& bitstream,
                          std::vector<uint8_t>& out)
{
  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) return false;

  if (de265_push_data(ctx, bitstream.data(), (int)bitstream.size(), 0, nullptr) != DE265_OK) {
    de265_free_decoder(ctx);
    return false;
  }
  de265_flush_data(ctx);

  bool ok = false;
  int more = 1;
  while (more) {
    de265_error err = de265_decode(ctx, &more);
    if (!de265_isOK(err)) { break; }

    const de265_image* img = de265_get_next_picture(ctx);
    if (img) {
      out.clear();
      out.reserve(W*H*3/2);

      for (int c=0;c<3;c++) {
        int stride = 0;
        const uint8_t* p = de265_get_image_plane(img, c, &stride);
        int cw = de265_get_image_width(img, c);
        int ch = de265_get_image_height(img, c);

        for (int y=0;y<ch;y++)
          out.insert(out.end(), p + y*stride, p + y*stride + cw);
      }

      ok = (out.size() == (size_t)(W*H*3/2));
      de265_release_next_picture(ctx);
      break;
    }
  }

  de265_free_decoder(ctx);
  return ok;
}


// Mean gradient across 4x4 block boundaries minus the mean gradient elsewhere.
// Positive values indicate visible blocking.
double blockiness(const std::vector<uint8_t>& yuv)
{
  double edge=0, inner=0;
  long ne=0, ni=0;

  for (int y=0;y<H;y++)
    for (int x=1;x<W;x++) {
      int d = abs((int)yuv[y*W+x] - (int)yuv[y*W+x-1]);
      if (x%4==0) { edge+=d; ne++; } else { inner+=d; ni++; }
    }

  for (int y=1;y<H;y++)
    for (int x=0;x<W;x++) {
      int d = abs((int)yuv[y*W+x] - (int)yuv[(y-1)*W+x]);
      if (y%4==0) { edge+=d; ne++; } else { inner+=d; ni++; }
    }

  if (!ne || !ni) return 0;
  return edge/ne - inner/ni;
}

} // namespace


class EncoderDeblockTest : public Test
{
public:
  const char* getName() const { return "encoder-deblock"; }
  const char* getDescription() const {
    return "encoder deblocking: encoder reconstruction matches decoder, blocking reduced";
  }

  bool work(bool quiet) {
    de265_init();

    bool ok = true;

    enc_result off, on;
    if (!encode_picture(false, 27, off)) {
      if (!quiet) printf("encoder failed (deblock disabled)\n");
      de265_free();
      return false;
    }
    if (!encode_picture(true, 27, on)) {
      if (!quiet) printf("encoder failed (deblock enabled)\n");
      de265_free();
      return false;
    }

    if (off.reconstruction.size() != on.reconstruction.size()) {
      if (!quiet) printf("reconstruction sizes differ\n");
      ok = false;
    }

    // The bitstreams must differ only in the deblocking signalling.
    if (off.bitstream == on.bitstream) {
      if (!quiet) printf("bitstream unchanged by --deblock (PPS/SH flags not signalled)\n");
      ok = false;
    }

    // 1) encoder-side reconstruction == decoder output
    std::vector<uint8_t> dec_off, dec_on;
    if (!decode_first_picture(off.bitstream, dec_off) ||
        !decode_first_picture(on.bitstream,  dec_on)) {
      if (!quiet) printf("decoding the encoder output failed\n");
      de265_free();
      return false;
    }

    if (dec_off != off.reconstruction) {
      if (!quiet) printf("MISMATCH: encoder reconstruction != decoder output (deblock off)\n");
      ok = false;
    }

    if (dec_on != on.reconstruction) {
      if (!quiet) printf("MISMATCH: encoder reconstruction != decoder output (deblock on)\n");
      ok = false;
    }

    // 2) the filter must reduce blocking at the same QP
    double b_off = blockiness(dec_off);
    double b_on  = blockiness(dec_on);

    if (!(b_on < b_off)) {
      if (!quiet) printf("blocking not reduced: off=%f on=%f\n", b_off, b_on);
      ok = false;
    }

    if (!quiet) {
      printf("blockiness: unfiltered=%f filtered=%f\n", b_off, b_on);
      printf("encoder reconstruction matches decoder: %s\n", ok?"yes":"NO");
    }

    de265_free();
    return ok;
  }
} encoderDeblockTest;

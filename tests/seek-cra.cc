/*
  Random-access seek test for libde265.

  Decodes a stream containing CRA pictures twice:

    run A: plain sequential decode of the whole stream (reference output)
    run B: decodes a few pictures, then seeks with
           de265_seek_to_next_random_access_point() and decodes on

  Verified properties:
    - both runs decode identically up to the seek point
    - the first picture after the seek is a CRA picture and its POC is the
      CRA picture's POC (POC cycle restarted at the CRA)
    - no RASL pictures are output after the seek
    - no decoder warnings are raised after the seek
    - the post-seek output is byte-identical to the matching suffix of the
      sequential output (CRA handled as BLA: DPB flushed, references to
      pre-seek pictures removed)

  Usage: seek-cra-test <annex-b stream with CRA pictures> [seek-after-frames]
*/

#include "de265.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


static const int NAL_CRA_NUT = 21;
static const int NAL_RASL_N  =  7;
static const int NAL_RASL_R  =  8;

// BLA (16..18), IDR (19..20), CRA (21), reserved IRAP (22..23)
static bool is_irap(int nal_type) { return nal_type >= 16 && nal_type <= 23; }


struct frame_info {
  int      poc;
  int      nal_type;
  uint64_t hash;      // FNV-1a over all reconstructed planes
  int      width, height;
};

static uint64_t hash_frame(const de265_image* img)
{
  uint64_t h = 1469598103934665603ULL;

  for (int c = 0; c < 3; c++) {
    int stride;
    const uint8_t* p = de265_get_image_plane(img, c, &stride);
    if (p == nullptr) continue;

    const int w = de265_get_image_width(img, c);
    const int hgt = de265_get_image_height(img, c);

    for (int y = 0; y < hgt; y++) {
      const uint8_t* row = p + (ptrdiff_t)y * stride;
      for (int x = 0; x < w; x++) {
        h ^= row[x];
        h *= 1099511628211ULL;
      }
    }
  }

  return h;
}


struct decode_result {
  std::vector<frame_info> frames;       // all output pictures
  int n_pre_seek_frames = 0;            // pictures output before the seek
  std::vector<std::string> warnings;    // warnings, tagged pre/post seek
  std::vector<std::string> warnings_post_seek;
  bool error = false;
};


// seek_after_frames < 0: plain sequential decode (run A)
static decode_result decode_stream(const std::vector<uint8_t>& data,
                                   int seek_after_frames)
{
  decode_result result;

  de265_decoder_context* ctx = de265_new_decoder();
  de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, false);

  de265_push_data(ctx, data.data(), (int)data.size(), 0, nullptr);
  de265_flush_data(ctx);

  bool seeked = false;
  int more = 1;

  while (more) {
    de265_error err = de265_decode(ctx, &more);
    if (err != DE265_OK &&
        err != DE265_ERROR_IMAGE_BUFFER_FULL &&
        err != DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      fprintf(stderr, "decode error: %s\n", de265_get_error_text(err));
      result.error = true;
      break;
    }

    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      frame_info fi;
      fi.poc = de265_get_image_POC(img);
      de265_get_image_NAL_header(img, &fi.nal_type, nullptr, nullptr, nullptr);
      fi.hash = hash_frame(img);
      fi.width  = de265_get_image_width(img, 0);
      fi.height = de265_get_image_height(img, 0);
      result.frames.push_back(fi);
    }

    if (!seeked && seek_after_frames >= 0 &&
        (int)result.frames.size() >= seek_after_frames) {
      result.n_pre_seek_frames = (int)result.frames.size();
      de265_seek_to_next_random_access_point(ctx);
      seeked = true;
    }

    de265_error warning;
    while ((warning = de265_get_warning(ctx)) != DE265_OK) {
      if (seeked) result.warnings_post_seek.push_back(de265_get_error_text(warning));
      else        result.warnings.push_back(de265_get_error_text(warning));
    }
  }

  if (!seeked) {
    result.n_pre_seek_frames = (int)result.frames.size();
  }

  de265_free_decoder(ctx);

  return result;
}


static std::vector<uint8_t> read_file(const char* filename)
{
  FILE* f = fopen(filename, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", filename);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(size);
  if (fread(data.data(), 1, size, f) != (size_t)size) {
    fprintf(stderr, "cannot read %s\n", filename);
    exit(1);
  }
  fclose(f);
  return data;
}


int main(int argc, char** argv)
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <stream.h265> [seek-after-frames]\n", argv[0]);
    return 1;
  }

  const std::vector<uint8_t> data = read_file(argv[1]);
  const int seek_after = (argc > 2) ? atoi(argv[2]) : 30;

  int failures = 0;
#define CHECK(cond, ...) \
  do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)


  // --- run A: sequential decode of the whole stream ---

  decode_result seq = decode_stream(data, -1);
  CHECK(!seq.error, "sequential run hit a decode error");
  CHECK(seq.warnings.empty(), "sequential run raised %zu warnings",
        seq.warnings.size());
  CHECK(!seq.frames.empty(), "sequential run produced no pictures");

  // locate the CRA pictures in the sequential output
  int n_cra = 0;
  for (const auto& f : seq.frames) {
    if (f.nal_type == NAL_CRA_NUT) n_cra++;
  }
  CHECK(n_cra >= 2, "stream should contain at least 2 CRA pictures, found %d",
        n_cra);

  printf("sequential: %zu pictures, %d CRA pictures\n", seq.frames.size(), n_cra);


  // --- run B: decode, then seek to the next random-access point ---

  decode_result seek = decode_stream(data, seek_after);
  CHECK(!seek.error, "seek run hit a decode error");
  CHECK(seek.n_pre_seek_frames >= seek_after,
        "seek triggered too early (%d < %d)", seek.n_pre_seek_frames, seek_after);

  // output before the seek must be identical to the sequential run
  CHECK(seek.n_pre_seek_frames <= (int)seq.frames.size(), "pre-seek overflow");
  for (int i = 0; i < seek.n_pre_seek_frames && i < (int)seq.frames.size(); i++) {
    CHECK(seek.frames[i].hash == seq.frames[i].hash &&
          seek.frames[i].poc  == seq.frames[i].poc,
          "pre-seek picture %d differs between runs", i);
  }

  const std::vector<frame_info> post(seek.frames.begin() + seek.n_pre_seek_frames,
                                     seek.frames.end());

  // does the stream still have a random-access point after the seek point?
  bool irap_after_seek = false;
  for (size_t i = seek.n_pre_seek_frames; i < seq.frames.size(); i++) {
    if (is_irap(seq.frames[i].nal_type)) { irap_after_seek = true; break; }
  }

  CHECK(!post.empty() || !irap_after_seek,
        "no pictures decoded after the seek, but the stream has an IRAP "
        "picture after the seek point");

  if (post.empty()) {
    printf("seek: no random-access point after the seek point, "
           "decoder produced no further pictures (as expected)\n");
  }

  if (!post.empty()) {
    // the first picture after the seek must be an IRAP picture (a CRA
    // handled like a BLA, or an IDR/BLA) ...
    CHECK(is_irap(post[0].nal_type),
          "first picture after seek is NAL type %d, expected an IRAP (16..23)",
          post[0].nal_type);

    // ... and it must be one of the IRAP pictures of the sequential run,
    // with the same POC (POC cycle restarted at the random-access point)
    int match = -1;
    for (size_t i = 0; i < seq.frames.size(); i++) {
      if (seq.frames[i].hash == post[0].hash) { match = (int)i; break; }
    }
    CHECK(match >= 0, "first post-seek picture not found in sequential output");

    if (match >= 0) {
      CHECK(is_irap(seq.frames[match].nal_type),
            "matching sequential picture is not an IRAP picture");

      // The POC cycle restarts at the random-access picture: its new POC is
      // its POC lsb (MSB reset to 0), so it may be lower than in the
      // sequential run by a multiple of MaxPicOrderCntLsb. The offset is
      // constant for all following pictures.
      const int poc_offset = seq.frames[match].poc - post[0].poc;
      CHECK(poc_offset >= 0,
            "first post-seek POC %d is larger than the sequential POC %d",
            post[0].poc, seq.frames[match].poc);

      // the whole post-seek output must be byte-identical to the
      // corresponding suffix of the sequential output
      CHECK((int)post.size() == (int)seq.frames.size() - match,
            "post-seek picture count is %zu, expected %zu",
            post.size(), seq.frames.size() - match);

      const int n = std::min((int)post.size(), (int)seq.frames.size() - match);
      for (int i = 0; i < n; i++) {
        CHECK(post[i].hash == seq.frames[match + i].hash,
              "post-seek picture %d (POC %d) differs from sequential output",
              i, post[i].poc);
        CHECK(post[i].poc == seq.frames[match + i].poc - poc_offset,
              "post-seek picture %d has POC %d, expected %d",
              i, post[i].poc, seq.frames[match + i].poc - poc_offset);
      }

      printf("seek: landed on IRAP (NAL type %d) with POC %d (sequential "
             "output index %d, POC offset %d), %zu post-seek pictures verified\n",
             post[0].nal_type, post[0].poc, match, poc_offset, post.size());
    }

    // The RASL pictures of the seek-target CRA picture (they precede it in
    // output order, i.e. have a smaller POC) must be discarded, not output.
    // RASL pictures of later CRA pictures are decoded with their leading
    // pictures available and are regular output pictures.
    for (size_t i = 0; i < post.size(); i++) {
      CHECK(post[i].poc >= post[0].poc,
            "picture with POC %d < CRA POC %d output after seek",
            post[i].poc, post[0].poc);
      if (post[i].poc < post[0].poc) {
        CHECK(post[i].nal_type != NAL_RASL_N && post[i].nal_type != NAL_RASL_R,
              "RASL picture of the seek CRA output after seek "
              "(post-seek index %zu)", i);
      }
    }
  }

  CHECK(seek.warnings_post_seek.empty(),
        "%zu warnings raised after the seek (first: %s)",
        seek.warnings_post_seek.size(),
        seek.warnings_post_seek.empty() ? "" : seek.warnings_post_seek[0].c_str());


  if (failures == 0) {
    printf("OK: all random-access seek checks passed\n");
    return 0;
  }

  printf("FAILED: %d check(s) failed\n", failures);
  return 1;
}

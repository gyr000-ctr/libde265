/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DE265_SEI_H
#define DE265_SEI_H

#include "libde265/bitstream.h"
#include "libde265/de265.h"


enum sei_payload_type {
  sei_payload_type_buffering_period = 0,
  sei_payload_type_pic_timing = 1,
  sei_payload_type_pan_scan_rect = 2,
  sei_payload_type_filler_payload = 3,
  sei_payload_type_user_data_registered_itu_t_t35 = 4,
  sei_payload_type_user_data_unregistered = 5,
  sei_payload_type_recovery_point = 6,
  sei_payload_type_scene_info = 9,
  sei_payload_type_picture_snapshot = 15,
  sei_payload_type_progressive_refinement_segment_start = 16,
  sei_payload_type_progressive_refinement_segment_end = 17,
  sei_payload_type_film_grain_characteristics = 19,
  sei_payload_type_post_filter_hint = 22,
  sei_payload_type_tone_mapping_info = 23,
  sei_payload_type_frame_packing_arrangement = 45,
  sei_payload_type_display_orientation = 47,
  sei_payload_type_structure_of_pictures_info = 128,
  sei_payload_type_active_parameter_sets = 129,
  sei_payload_type_decoding_unit_info = 130,
  sei_payload_type_temporal_sub_layer_zero_index = 131,
  sei_payload_type_decoded_picture_hash = 132,
  sei_payload_type_scalable_nesting = 133,
  sei_payload_type_region_refresh_info = 134,
  sei_payload_type_no_display = 135,
  sei_payload_type_motion_constrained_tile_sets = 136,
  sei_payload_type_mastering_display_colour_volume = 137,
  sei_payload_type_content_light_level_info = 144
};


enum sei_decoded_picture_hash_type {
  sei_decoded_picture_hash_type_MD5 = 0,
  sei_decoded_picture_hash_type_CRC = 1,
  sei_decoded_picture_hash_type_checksum = 2
};


typedef struct {
  enum sei_decoded_picture_hash_type hash_type;
  uint8_t md5[3][16];
  uint16_t crc[3];
  uint32_t checksum[3];
} sei_decoded_picture_hash;


// SEI type 137: mastering display colour volume.
// Chromaticities are coded as u(16) in units of 1/50000,
// luminance values as u(32) in units of 1/10000 cd/m^2.
typedef struct {
  uint16_t display_primaries_x[3];
  uint16_t display_primaries_y[3];
  uint16_t white_point_x;
  uint16_t white_point_y;
  uint32_t max_display_mastering_luminance;
  uint32_t min_display_mastering_luminance;
} sei_mastering_display_colour_volume;


// SEI type 144: content light level information.
typedef struct {
  uint16_t max_content_light_level;        // cd/m^2
  uint16_t max_pic_average_light_level;    // cd/m^2
} sei_content_light_level;


typedef struct {
  uint16_t payload_type; // enum sei_payload_type
  uint32_t payload_size;

  // true when the message came from a suffix SEI NAL (NAL type 40),
  // false for a prefix SEI NAL (NAL type 39).
  bool suffix = false;

  union {
    sei_decoded_picture_hash decoded_picture_hash;
    sei_mastering_display_colour_volume mastering_display;
    sei_content_light_level content_light_level;
  } data;
} sei_message;

class seq_parameter_set;

const char* sei_type_name(uint16_t type);

de265_error read_sei(bitreader* reader, sei_message*, bool suffix, const seq_parameter_set* sps);
void dump_sei(const sei_message*, const seq_parameter_set* sps);
de265_error process_sei(const sei_message*, struct de265_image* img);

#endif

/*****************************************************************************
 * tospdif.c : encapsulates A/52 and DTS frames into S/PDIF packets
 *****************************************************************************
 * Copyright (C) 2002, 2006-2016 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Stéphane Borel <stef@via.ecp.fr>
 *          Rémi Denis-Courmont
 *          Rafaël Carré
 *          Thomas Guillem
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>

#include <vlc_aout.h>
#include <vlc_filter.h>

#include "../packetizer/a52.h"
#include "../packetizer/dts_header.h"

static int  Open( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_MISC )
    set_description( N_("Audio filter for A/52/DTS->S/PDIF encapsulation") )
    set_capability( "audio converter", 10 )
    set_callbacks( Open, Close )
vlc_module_end ()

struct filter_sys_t
{
    block_t *p_out_buf;
    size_t i_out_offset;

    union
    {
        struct
        {
            unsigned int i_nb_blocks_substream0;
        } eac3;
        struct
        {
            unsigned int i_frame_count;
        } truehd;
    } spec;
};

#define SPDIF_HEADER_SIZE 8

#define IEC61937_AC3 0x01
#define IEC61937_EAC3 0x15
#define IEC61937_TRUEHD 0x16
#define IEC61937_DTS1 0x0B
#define IEC61937_DTS2 0x0C
#define IEC61937_DTS3 0x0D

#define SPDIF_MORE_DATA 1
#define SPDIF_SUCCESS VLC_SUCCESS
#define SPDIF_ERROR VLC_EGENERIC

static bool is_big_endian( filter_t *p_filter, block_t *p_in_buf )
{
    switch( p_filter->fmt_in.audio.i_format )
    {
        case VLC_CODEC_A52:
        case VLC_CODEC_EAC3:
        case VLC_CODEC_MLP:
        case VLC_CODEC_TRUEHD:
            return true;
        case VLC_CODEC_DTS:
            return p_in_buf->p_buffer[0] == 0x1F
                || p_in_buf->p_buffer[0] == 0x7F;
        default:
            vlc_assert_unreachable();
    }
}

static inline void write_16( filter_t *p_filter, void *p_buf, uint16_t i_val )
{
    if( p_filter->fmt_out.audio.i_format == VLC_CODEC_SPDIFB )
        SetWBE( p_buf, i_val );
    else
        SetWLE( p_buf, i_val );
}

static void write_padding( filter_t *p_filter, size_t i_size )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    assert( p_sys->p_out_buf != NULL );

    assert( p_sys->p_out_buf->i_buffer - p_sys->i_out_offset >= i_size );

    uint8_t *p_out = &p_sys->p_out_buf->p_buffer[p_sys->i_out_offset];
    memset( p_out, 0, i_size );
    p_sys->i_out_offset += i_size;
}

static void write_data( filter_t *p_filter, const void *p_buf, size_t i_size,
                        bool b_input_big_endian )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    assert( p_sys->p_out_buf != NULL );

    bool b_output_big_endian =
        p_filter->fmt_out.audio.i_format == VLC_CODEC_SPDIFB;
    uint8_t *p_out = &p_sys->p_out_buf->p_buffer[p_sys->i_out_offset];
    const uint8_t *p_in = p_buf;

    assert( p_sys->p_out_buf->i_buffer - p_sys->i_out_offset >= i_size );

    if( b_input_big_endian != b_output_big_endian )
        swab( p_in, p_out, i_size & ~1 );
    else
        memcpy( p_out, p_in, i_size & ~1 );
    p_sys->i_out_offset += ( i_size & ~1 );

    if( i_size & 1 )
    {
        assert( p_sys->p_out_buf->i_buffer - p_sys->i_out_offset >= 2 );
        p_out += ( i_size & ~1 );
        write_16( p_filter, p_out, p_in[i_size - 1] << 8 );
        p_sys->i_out_offset += 2;
    }
}

static void write_buffer( filter_t *p_filter, block_t *p_in_buf )
{
    write_data( p_filter, p_in_buf->p_buffer, p_in_buf->i_buffer,
                is_big_endian( p_filter, p_in_buf ) );
    p_filter->p_sys->p_out_buf->i_length += p_in_buf->i_length;
}

static int write_init( filter_t *p_filter, block_t *p_in_buf,
                       size_t i_out_size, unsigned i_nb_samples )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    assert( p_sys->p_out_buf == NULL );
    assert( i_out_size > SPDIF_HEADER_SIZE && ( i_out_size & 3 ) == 0 );

    p_sys->p_out_buf = block_Alloc( i_out_size );
    if( !p_sys->p_out_buf )
        return VLC_ENOMEM;
    p_sys->p_out_buf->i_dts = p_in_buf->i_dts;
    p_sys->p_out_buf->i_pts = p_in_buf->i_pts;
    p_sys->p_out_buf->i_nb_samples = i_nb_samples;

    p_sys->i_out_offset = SPDIF_HEADER_SIZE; /* Place for the S/PDIF header */
    return VLC_SUCCESS;
}

static void write_finalize( filter_t *p_filter, uint16_t i_data_type,
                            uint8_t i_length_mul )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    assert( p_sys->p_out_buf != NULL );
    uint8_t *p_out = p_sys->p_out_buf->p_buffer;

    /* S/PDIF header */
    if( i_data_type != 0 )
    {
        assert( p_sys->i_out_offset > SPDIF_HEADER_SIZE );
        assert( i_length_mul == 1 || i_length_mul == 8 );

        write_16( p_filter, &p_out[0], 0xf872 ); /* syncword 1 */
        write_16( p_filter, &p_out[2], 0x4e1f ); /* syncword 2 */
        write_16( p_filter, &p_out[4], i_data_type ); /* data type */
        /* length in bits or bytes */
        write_16( p_filter, &p_out[6],
                  ( p_sys->i_out_offset - SPDIF_HEADER_SIZE ) * i_length_mul );
    }

    /* 0 padding */
    if( p_sys->i_out_offset < p_sys->p_out_buf->i_buffer )
        write_padding( p_filter,
                       p_sys->p_out_buf->i_buffer - p_sys->i_out_offset );
}

static int write_buffer_ac3( filter_t *p_filter, block_t *p_in_buf )
{
    static const size_t a52_size = A52_FRAME_NB * 4;

    if( unlikely( p_in_buf->i_buffer < 6
     || p_in_buf->i_buffer > a52_size
     || p_in_buf->i_nb_samples != A52_FRAME_NB ) )
    {
        /* Input is not correctly packetizer. Try to parse the buffer in order
         * to get the mandatory informations to play AC3 over S/PDIF */
        vlc_a52_header_t a52;
        if( vlc_a52_header_Parse( &a52, p_in_buf->p_buffer, p_in_buf->i_buffer )
            != VLC_SUCCESS || a52.b_eac3 || a52.i_size > p_in_buf->i_buffer )
            return SPDIF_ERROR;
        p_in_buf->i_buffer = a52.i_size;
        p_in_buf->i_nb_samples = a52.i_samples;
    }

    if( p_in_buf->i_buffer + SPDIF_HEADER_SIZE > a52_size
     || write_init( p_filter, p_in_buf, a52_size, A52_FRAME_NB ) )
        return SPDIF_ERROR;
    write_buffer( p_filter, p_in_buf );
    write_finalize( p_filter, IEC61937_AC3 |
                    ( ( p_in_buf->p_buffer[5] & 0x7 ) << 8 ) /* bsmod */,
                    8 /* in bits */ );

    return SPDIF_SUCCESS;
}

static int write_buffer_eac3( filter_t *p_filter, block_t *p_in_buf )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    vlc_a52_header_t a52 = { };
    if( vlc_a52_header_Parse( &a52, p_in_buf->p_buffer, p_in_buf->i_buffer )
        != VLC_SUCCESS || a52.i_size > p_in_buf->i_buffer )
        return SPDIF_ERROR;

    p_in_buf->i_buffer = a52.i_size;
    p_in_buf->i_nb_samples = a52.i_samples;

    if( !p_sys->p_out_buf
     && write_init( p_filter, p_in_buf, AOUT_SPDIF_SIZE * 4, AOUT_SPDIF_SIZE ) )
        return SPDIF_ERROR;
    if( p_in_buf->i_buffer > p_sys->p_out_buf->i_buffer - p_sys->i_out_offset )
        return SPDIF_ERROR;

    write_buffer( p_filter, p_in_buf );

    if( a52.b_eac3 )
    {
        if( ( a52.eac3.strmtyp == EAC3_STRMTYP_INDEPENDENT
           || a52.eac3.strmtyp == EAC3_STRMTYP_AC3_CONVERT )
         && a52.i_blocks_per_sync_frame != 6 )
        {
            /* cf. Annex E 2.3.1.2 of AC3 spec */
            if( a52.eac3.i_substreamid == 0 )
                p_sys->spec.eac3.i_nb_blocks_substream0
                    += a52.i_blocks_per_sync_frame;

            if( p_sys->spec.eac3.i_nb_blocks_substream0 != 6 )
                return SPDIF_MORE_DATA;
            else
                p_sys->spec.eac3.i_nb_blocks_substream0 = 0;
        }
        write_finalize( p_filter, IEC61937_EAC3, 1 /* in bytes */ );
        return SPDIF_SUCCESS;
    }
    else
        return SPDIF_MORE_DATA;

}

/* Adapted from libavformat/spdifenc.c:
 * It seems Dolby TrueHD frames have to be encapsulated in MAT frames before
 * they can be encapsulated in IEC 61937.
 * Here we encapsulate 24 TrueHD frames in a single MAT frame, padding them
 * to achieve constant rate.
 * The actual format of a MAT frame is unknown, but the below seems to work.
 * However, it seems it is not actually necessary for the 24 TrueHD frames to
 * be in an exact alignment with the MAT frame
 */
static int write_buffer_truehd( filter_t *p_filter, block_t *p_in_buf )
{
#define TRUEHD_FRAME_OFFSET     2560

    filter_sys_t *p_sys = p_filter->p_sys;

    if( !p_sys->p_out_buf
     && write_init( p_filter, p_in_buf, 61440, 61440 / 16 ) )
        return SPDIF_ERROR;

    int i_padding = 0;
    if( p_sys->spec.truehd.i_frame_count == 0 )
    {
        static const char p_mat_start_code[20] = {
            0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
            0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0
        };
        write_data( p_filter, p_mat_start_code, 20, true );
        /* We need to include the S/PDIF header in the first MAT frame */
        i_padding = TRUEHD_FRAME_OFFSET - p_in_buf->i_buffer - 20
                  - SPDIF_HEADER_SIZE;
    }
    else if( p_sys->spec.truehd.i_frame_count == 11 )
    {
        /* The middle mat code need to be at the ((2560 * 12) - 4) offset */
        i_padding = TRUEHD_FRAME_OFFSET - p_in_buf->i_buffer - 4;
    }
    else if( p_sys->spec.truehd.i_frame_count == 12 )
    {
        static const char p_mat_middle_code[12] = {
            0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA,
            0x82, 0x83, 0x49, 0x80, 0x77, 0xE0
        };
        write_data( p_filter, p_mat_middle_code, 12, true );
        i_padding = TRUEHD_FRAME_OFFSET - p_in_buf->i_buffer - ( 12 - 4 );
    }
    else if( p_sys->spec.truehd.i_frame_count == 23 )
    {
        static const char p_mat_end_code[16] = {
            0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11
        };

        /* The end mat code need to be at the ((2560 * 24) - 24) offset */
        i_padding = TRUEHD_FRAME_OFFSET - p_in_buf->i_buffer - 24;

        if( i_padding < 0 || p_in_buf->i_buffer + i_padding >
            p_sys->p_out_buf->i_buffer - p_sys->i_out_offset )
            return SPDIF_ERROR;

        write_buffer( p_filter, p_in_buf );
        write_padding( p_filter, i_padding );
        write_data( p_filter, p_mat_end_code, 16, true );
        write_finalize( p_filter, IEC61937_TRUEHD, 1 /* in bytes */ );
        p_sys->spec.truehd.i_frame_count = 0;
        return SPDIF_SUCCESS;
    }
    else
        i_padding = TRUEHD_FRAME_OFFSET - p_in_buf->i_buffer;

    if( i_padding < 0 || p_in_buf->i_buffer + i_padding >
        p_sys->p_out_buf->i_buffer - p_sys->i_out_offset )
        return SPDIF_ERROR;

    write_buffer( p_filter, p_in_buf );
    write_padding( p_filter, i_padding );
    p_sys->spec.truehd.i_frame_count++;
    return SPDIF_MORE_DATA;
}

static int write_buffer_dts( filter_t *p_filter, block_t *p_in_buf )
{
    uint16_t i_data_type;
    if( p_in_buf->i_nb_samples == 0 )
    {
        /* Input is not correctly packetizer. Try to parse the buffer in order
         * to get the mandatory informations to play DTS over S/PDIF */
        vlc_dts_header_t header;
        if( vlc_dts_header_Parse( &header, p_in_buf->p_buffer,
                                  p_in_buf->i_buffer ) != VLC_SUCCESS )
            return SPDIF_ERROR;
        p_in_buf->i_nb_samples = header.i_frame_length;
        p_in_buf->i_buffer = header.i_frame_size;
    }
    switch( p_in_buf->i_nb_samples )
    {
    case  512:
        i_data_type = IEC61937_DTS1;
        break;
    case 1024:
        i_data_type = IEC61937_DTS2;
        break;
    case 2048:
        i_data_type = IEC61937_DTS3;
        break;
    default:
        msg_Err( p_filter, "Frame size %d not supported",
                 p_in_buf->i_nb_samples );
        return SPDIF_ERROR;
    }

    if( p_in_buf->i_buffer == p_in_buf->i_nb_samples * 4 )
    {
        /* No enough room to put the S/PDIF header. This is the case for DTS
         * inside WAV. */
        i_data_type = 0;
    }
    else if( p_in_buf->i_buffer + SPDIF_HEADER_SIZE > p_in_buf->i_nb_samples * 4 )
        return SPDIF_ERROR;

    if( write_init( p_filter, p_in_buf, p_in_buf->i_nb_samples * 4,
                    p_in_buf->i_nb_samples ) )
        return SPDIF_ERROR;

    if( i_data_type == 0 )
        p_filter->p_sys->i_out_offset = 0;

    write_buffer( p_filter, p_in_buf );
    write_finalize( p_filter, i_data_type, 8 /* in bits */ );
    return SPDIF_SUCCESS;
}

static void Flush( filter_t *p_filter )
{
    filter_sys_t *p_sys = p_filter->p_sys;

    if( p_sys->p_out_buf != NULL )
    {
        block_Release( p_sys->p_out_buf );
        p_sys->p_out_buf = NULL;
    }
    memset( &p_sys->spec, 0, sizeof( p_sys->spec ) );
}

static block_t *DoWork( filter_t *p_filter, block_t *p_in_buf )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    block_t *p_out_buf = NULL;

    int i_ret;
    switch( p_filter->fmt_in.audio.i_format )
    {
        case VLC_CODEC_A52:
            i_ret = write_buffer_ac3( p_filter, p_in_buf );
            break;
        case VLC_CODEC_EAC3:
            i_ret = write_buffer_eac3( p_filter, p_in_buf );
            break;
        case VLC_CODEC_MLP:
        case VLC_CODEC_TRUEHD:
            i_ret = write_buffer_truehd( p_filter, p_in_buf );
            break;
        case VLC_CODEC_DTS:
            i_ret = write_buffer_dts( p_filter, p_in_buf );
            break;
        default:
            vlc_assert_unreachable();
    }

    switch( i_ret )
    {
        case SPDIF_SUCCESS:
            assert( p_sys->p_out_buf->i_buffer == p_sys->i_out_offset );
            p_out_buf = p_sys->p_out_buf;
            p_sys->p_out_buf = NULL;
            break;
        case SPDIF_MORE_DATA:
            break;
        case SPDIF_ERROR:
            Flush( p_filter );
            break;
    }

    block_Release( p_in_buf );
    return p_out_buf;
}

static int Open( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys;

    if( ( p_filter->fmt_in.audio.i_format != VLC_CODEC_DTS &&
          p_filter->fmt_in.audio.i_format != VLC_CODEC_A52 &&
          p_filter->fmt_in.audio.i_format != VLC_CODEC_EAC3 &&
          p_filter->fmt_in.audio.i_format != VLC_CODEC_MLP &&
          p_filter->fmt_in.audio.i_format != VLC_CODEC_TRUEHD ) ||
        ( p_filter->fmt_out.audio.i_format != VLC_CODEC_SPDIFL &&
          p_filter->fmt_out.audio.i_format != VLC_CODEC_SPDIFB ) )
        return VLC_EGENERIC;

    p_sys = p_filter->p_sys = malloc( sizeof(filter_sys_t) );
    if( unlikely( p_sys == NULL ) )
        return VLC_ENOMEM;
    p_sys->p_out_buf = NULL;

    memset( &p_sys->spec, 0, sizeof( p_sys->spec ) );

    p_filter->pf_audio_filter = DoWork;
    p_filter->pf_flush = Flush;

    return VLC_SUCCESS;
}

static void Close( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;

    Flush( p_filter );
    free( p_filter->p_sys );
}

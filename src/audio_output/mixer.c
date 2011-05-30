/*****************************************************************************
 * mixer.c : audio output mixing operations
 *****************************************************************************
 * Copyright (C) 2002-2004 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <assert.h>

#include <stddef.h>
#include <vlc_common.h>
#include <libvlc.h>
#include <vlc_modules.h>

#include <vlc_aout.h>
#include "aout_internal.h"
/*****************************************************************************
 * aout_MixerNew: prepare a mixer plug-in
 *****************************************************************************
 * Please note that you must hold the mixer lock.
 *****************************************************************************/
int aout_MixerNew( aout_instance_t * p_aout )
{
    assert( !p_aout->p_mixer );
    vlc_assert_locked( &p_aout->input_fifos_lock );

    aout_mixer_t *p_mixer = vlc_object_create( p_aout, sizeof(*p_mixer) );
    if( !p_mixer )
        return VLC_EGENERIC;

    p_mixer->fmt = p_aout->mixer_format;
    p_mixer->b_alloc = true;
    p_mixer->multiplier = p_aout->mixer_multiplier;
    p_mixer->input = &p_aout->pp_inputs[0]->mixer;
    p_mixer->mix = NULL;
    p_mixer->sys = NULL;

    p_mixer->module = module_need( p_mixer, "audio mixer", NULL, false );
    if( !p_mixer->module )
    {
        msg_Err( p_aout, "no suitable audio mixer" );
        vlc_object_release( p_mixer );
        return VLC_EGENERIC;
    }

    /* */
    p_aout->p_mixer = p_mixer;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * aout_MixerDelete: delete the mixer
 *****************************************************************************
 * Please note that you must hold the mixer lock.
 *****************************************************************************/
void aout_MixerDelete( aout_instance_t * p_aout )
{
    if( !p_aout->p_mixer )
        return;

    module_unneed( p_aout->p_mixer, p_aout->p_mixer->module );

    vlc_object_release( p_aout->p_mixer );

    /* */
    p_aout->p_mixer = NULL;
}

/*****************************************************************************
 * MixBuffer: try to prepare one output buffer
 *****************************************************************************
 * Please note that you must hold the mixer lock.
 *****************************************************************************/
static int MixBuffer( aout_instance_t * p_aout )
{
    int             i, i_first_input = 0;
    mtime_t start_date, end_date;
    date_t  exact_start_date;

    if( !p_aout->p_mixer )
    {
        /* Free all incoming buffers. */
        aout_lock_input_fifos( p_aout );
        for ( i = 0; i < p_aout->i_nb_inputs; i++ )
        {
            aout_input_t * p_input = p_aout->pp_inputs[i];
            aout_buffer_t * p_buffer = p_input->mixer.fifo.p_first;
            if ( p_input->b_error ) continue;
            while ( p_buffer != NULL )
            {
                aout_buffer_t * p_next = p_buffer->p_next;
                aout_BufferFree( p_buffer );
                p_buffer = p_next;
            }
        }
        aout_unlock_input_fifos( p_aout );
        return -1;
    }


    aout_lock_input_fifos( p_aout );
    aout_lock_output_fifo( p_aout );

    /* Retrieve the date of the next buffer. */
    exact_start_date = p_aout->output.fifo.end_date;
    start_date = date_Get( &exact_start_date );

    if ( start_date != 0 && start_date < mdate() )
    {
        /* The output is _very_ late. This can only happen if the user
         * pauses the stream (or if the decoder is buggy, which cannot
         * happen :). */
        msg_Warn( p_aout, "output PTS is out of range (%"PRId64"), clearing out",
                  mdate() - start_date );
        aout_FifoSet( p_aout, &p_aout->output.fifo, 0 );
        date_Set( &exact_start_date, 0 );
        start_date = 0;
    }

    aout_unlock_output_fifo( p_aout );

    /* See if we have enough data to prepare a new buffer for the audio
     * output. First : start date. */
    if ( !start_date )
    {
        /* Find the latest start date available. */
        for ( i = 0; i < p_aout->i_nb_inputs; i++ )
        {
            aout_input_t * p_input = p_aout->pp_inputs[i];
            aout_fifo_t * p_fifo = &p_input->mixer.fifo;
            aout_buffer_t * p_buffer;

            if ( p_input->b_error || p_input->b_paused )
                continue;

            p_buffer = p_fifo->p_first;
            while ( p_buffer != NULL && p_buffer->i_pts < mdate() )
            {
                msg_Warn( p_aout, "input PTS is out of range (%"PRId64"), "
                          "trashing", mdate() - p_buffer->i_pts );
                p_buffer = aout_FifoPop( p_aout, p_fifo );
                aout_BufferFree( p_buffer );
                p_buffer = p_fifo->p_first;
                p_input->mixer.begin = NULL;
            }

            if ( p_buffer == NULL )
            {
                break;
            }

            if ( !start_date || start_date < p_buffer->i_pts )
            {
                date_Set( &exact_start_date, p_buffer->i_pts );
                start_date = p_buffer->i_pts;
            }
        }

        if ( i < p_aout->i_nb_inputs )
        {
            /* Interrupted before the end... We can't run. */
            aout_unlock_input_fifos( p_aout );
            return -1;
        }
    }
    date_Increment( &exact_start_date, p_aout->output.i_nb_samples );
    end_date = date_Get( &exact_start_date );

    /* Check that start_date and end_date are available for all input
     * streams. */
    for ( i = 0; i < p_aout->i_nb_inputs; i++ )
    {
        aout_input_t * p_input = p_aout->pp_inputs[i];
        aout_fifo_t * p_fifo = &p_input->mixer.fifo;
        aout_buffer_t * p_buffer;
        mtime_t prev_date;
        bool b_drop_buffers;

        p_input->mixer.is_invalid = p_input->b_error || p_input->b_paused;
        if ( p_input->mixer.is_invalid )
        {
            if ( i_first_input == i ) i_first_input++;
            continue;
        }

        p_buffer = p_fifo->p_first;
        if ( p_buffer == NULL )
        {
            break;
        }

        /* Check for the continuity of start_date */
        while ( p_buffer != NULL
             && p_buffer->i_pts + p_buffer->i_length < start_date - 1 )
        {
            /* We authorize a +-1 because rounding errors get compensated
             * regularly. */
            aout_buffer_t * p_next = p_buffer->p_next;
            msg_Warn( p_aout, "the mixer got a packet in the past (%"PRId64")",
                      start_date - (p_buffer->i_pts + p_buffer->i_length) );
            aout_BufferFree( p_buffer );
            p_fifo->p_first = p_buffer = p_next;
            p_input->mixer.begin = NULL;
        }
        if ( p_buffer == NULL )
        {
            p_fifo->pp_last = &p_fifo->p_first;
            break;
        }

        /* Check that we have enough samples. */
        for ( ; ; )
        {
            p_buffer = p_fifo->p_first;
            if ( p_buffer == NULL ) break;
            if ( p_buffer->i_pts + p_buffer->i_length >= end_date ) break;

            /* Check that all buffers are contiguous. */
            prev_date = p_fifo->p_first->i_pts + p_fifo->p_first->i_length;
            p_buffer = p_buffer->p_next;
            b_drop_buffers = 0;
            for ( ; p_buffer != NULL; p_buffer = p_buffer->p_next )
            {
                if ( prev_date != p_buffer->i_pts )
                {
                    msg_Warn( p_aout,
                              "buffer hole, dropping packets (%"PRId64")",
                              p_buffer->i_pts - prev_date );
                    b_drop_buffers = 1;
                    break;
                }
                if ( p_buffer->i_pts + p_buffer->i_length >= end_date ) break;
                prev_date = p_buffer->i_pts + p_buffer->i_length;
            }
            if ( b_drop_buffers )
            {
                aout_buffer_t * p_deleted = p_fifo->p_first;
                while ( p_deleted != NULL && p_deleted != p_buffer )
                {
                    aout_buffer_t * p_next = p_deleted->p_next;
                    aout_BufferFree( p_deleted );
                    p_deleted = p_next;
                }
                p_fifo->p_first = p_deleted; /* == p_buffer */
            }
            else break;
        }
        if ( p_buffer == NULL ) break;

        p_buffer = p_fifo->p_first;
        if ( !AOUT_FMT_NON_LINEAR( &p_aout->p_mixer->fmt ) )
        {
            /* Additionally check that p_first_byte_to_mix is well
             * located. */
            mtime_t i_buffer = (start_date - p_buffer->i_pts)
                            * p_aout->p_mixer->fmt.i_bytes_per_frame
                            * p_aout->p_mixer->fmt.i_rate
                            / p_aout->p_mixer->fmt.i_frame_length
                            / 1000000;
            ptrdiff_t mixer_nb_bytes;

            if ( p_input->mixer.begin == NULL )
            {
                p_input->mixer.begin = p_buffer->p_buffer;
            }
            mixer_nb_bytes = p_input->mixer.begin - p_buffer->p_buffer;

            if ( !((i_buffer + p_aout->p_mixer->fmt.i_bytes_per_frame
                     > mixer_nb_bytes) &&
                   (i_buffer < p_aout->p_mixer->fmt.i_bytes_per_frame
                     + mixer_nb_bytes)) )
            {
                msg_Warn( p_aout, "mixer start isn't output start (%"PRId64")",
                          i_buffer - mixer_nb_bytes );

                /* Round to the nearest multiple */
                i_buffer /= p_aout->p_mixer->fmt.i_bytes_per_frame;
                i_buffer *= p_aout->p_mixer->fmt.i_bytes_per_frame;
                if( i_buffer < 0 )
                {
                    /* Is it really the best way to do it ? */
                    aout_lock_output_fifo( p_aout );
                    aout_FifoSet( p_aout, &p_aout->output.fifo, 0 );
                    date_Set( &exact_start_date, 0 );
                    aout_unlock_output_fifo( p_aout );
                    break;
                }

                p_input->mixer.begin = p_buffer->p_buffer + i_buffer;
            }
        }
    }

    if ( i < p_aout->i_nb_inputs || i_first_input == p_aout->i_nb_inputs )
    {
        /* Interrupted before the end... We can't run. */
        aout_unlock_input_fifos( p_aout );
        return -1;
    }

    /* Run the mixer. */
    aout_buffer_t * p_outbuf;

    if( p_aout->p_mixer->b_alloc )
    {
        p_outbuf = block_Alloc( p_aout->output.i_nb_samples
                              * p_aout->p_mixer->fmt.i_bytes_per_frame
                              / p_aout->p_mixer->fmt.i_frame_length );
        if( likely(p_outbuf != NULL) )
            p_outbuf->i_nb_samples = p_aout->output.i_nb_samples;
    }
    else
        p_outbuf = p_aout->pp_inputs[i_first_input]->mixer.fifo.p_first;
    if ( p_outbuf == NULL )
    {
        aout_unlock_input_fifos( p_aout );
        return -1;
    }
    p_outbuf->i_pts = start_date;
    p_outbuf->i_length = end_date - start_date;

    p_aout->p_mixer->mix( p_aout->p_mixer, p_outbuf );

    aout_unlock_input_fifos( p_aout );

    aout_OutputPlay( p_aout, p_outbuf );

    return 0;
}

/*****************************************************************************
 * aout_MixerRun: entry point for the mixer & post-filters processing
 *****************************************************************************
 * Please note that you must hold the mixer lock.
 *****************************************************************************/
void aout_MixerRun( aout_instance_t * p_aout )
{
    while( MixBuffer( p_aout ) != -1 );
}

/*****************************************************************************
 * aout_MixerMultiplierSet: set p_aout->mixer.f_multiplier
 *****************************************************************************
 * Please note that we assume that you own the mixer lock when entering this
 * function. This function returns -1 on error.
 *****************************************************************************/
void aout_MixerMultiplierSet( aout_instance_t * p_aout, float f_multiplier )
{
    p_aout->mixer_multiplier = f_multiplier;
    if( p_aout->p_mixer )
        p_aout->p_mixer->multiplier = f_multiplier;
}

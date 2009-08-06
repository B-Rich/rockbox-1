/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002-2007 Björn Stenberg
 * Copyright (C) 2007-2008 Nicolas Pennequin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "font.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "action.h"
#include "system.h"
#include "settings.h"
#include "settings_list.h"
#include "rbunicode.h"
#include "timefuncs.h"
#include "audio.h"
#include "status.h"
#include "power.h"
#include "powermgmt.h"
#include "sound.h"
#include "debug.h"
#ifdef HAVE_LCD_CHARCELLS
#include "hwcompat.h"
#endif
#include "abrepeat.h"
#include "mp3_playback.h"
#include "lang.h"
#include "misc.h"
#include "led.h"
#ifdef HAVE_LCD_BITMAP
/* Image stuff */
#include "albumart.h"
#endif
#include "dsp.h"
#include "playlist.h"
#if CONFIG_CODEC == SWCODEC
#include "playback.h"
#endif
#include "viewport.h"

#include "wps_internals.h"
#include "wps.h"

static char* get_codectype(const struct mp3entry* id3)
{
    if (id3->codectype < AFMT_NUM_CODECS) {
        return (char*)audio_formats[id3->codectype].label;
    } else {
        return NULL;
    }
}

/* Extract a part from a path.
 *
 * buf      - buffer extract part to.
 * buf_size - size of buffer.
 * path     - path to extract from.
 * level    - what to extract. 0 is file name, 1 is parent of file, 2 is
 *            parent of parent, etc.
 *
 * Returns buf if the desired level was found, NULL otherwise.
 */
static char* get_dir(char* buf, int buf_size, const char* path, int level)
{
    const char* sep;
    const char* last_sep;
    int len;

    sep = path + strlen(path);
    last_sep = sep;

    while (sep > path)
    {
        if ('/' == *(--sep))
        {
            if (!level)
                break;

            level--;
            last_sep = sep - 1;
        }
    }

    if (level || (last_sep <= sep))
        return NULL;

    len = MIN(last_sep - sep, buf_size - 1);
    strlcpy(buf, sep + 1, len + 1);
    return buf;
}

/* Return the tag found at index i and write its value in buf.
   The return value is buf if the tag had a value, or NULL if not.

   intval is used with conditionals/enums: when this function is called,
   intval should contain the number of options in the conditional/enum.
   When this function returns, intval is -1 if the tag is non numeric or,
   if the tag is numeric, *intval is the enum case we want to go to (between 1
   and the original value of *intval, inclusive).
   When not treating a conditional/enum, intval should be NULL.
*/
const char *get_token_value(struct gui_wps *gwps,
                           struct wps_token *token,
                           char *buf, int buf_size,
                           int *intval)
{
    if (!gwps)
        return NULL;

    struct wps_data *data = gwps->data;
    struct wps_state *state = gwps->state;

    if (!data || !state)
        return NULL;

    struct mp3entry *id3;

    if (token->next)
        id3 = state->nid3;
    else
        id3 = state->id3;

    if (!id3)
        return NULL;

#if CONFIG_RTC
    struct tm* tm = NULL;

    /* if the token is an RTC one, update the time
       and do the necessary checks */
    if (token->type >= WPS_TOKENS_RTC_BEGIN
        && token->type <= WPS_TOKENS_RTC_END)
    {
        tm = get_time();

        if (!valid_time(tm))
            return NULL;
    }
#endif

    int limit = 1;
    if (intval)
    {
        limit = *intval;
        *intval = -1;
    }

    switch (token->type)
    {
        case WPS_TOKEN_CHARACTER:
            return &(token->value.c);

        case WPS_TOKEN_STRING:
            return data->strings[token->value.i];

        case WPS_TOKEN_TRACK_TIME_ELAPSED:
            format_time(buf, buf_size,
                        id3->elapsed + state->ff_rewind_count);
            return buf;

        case WPS_TOKEN_TRACK_TIME_REMAINING:
            format_time(buf, buf_size,
                        id3->length - id3->elapsed -
                        state->ff_rewind_count);
            return buf;

        case WPS_TOKEN_TRACK_LENGTH:
            format_time(buf, buf_size, id3->length);
            return buf;

        case WPS_TOKEN_PLAYLIST_ENTRIES:
            snprintf(buf, buf_size, "%d", playlist_amount());
            return buf;

        case WPS_TOKEN_PLAYLIST_NAME:
            return playlist_name(NULL, buf, buf_size);

        case WPS_TOKEN_PLAYLIST_POSITION:
            snprintf(buf, buf_size, "%d", playlist_get_display_index());
            return buf;

        case WPS_TOKEN_PLAYLIST_SHUFFLE:
            if ( global_settings.playlist_shuffle )
                return "s";
            else
                return NULL;
            break;

        case WPS_TOKEN_VOLUME:
            snprintf(buf, buf_size, "%d", global_settings.volume);
            if (intval)
            {
                if (global_settings.volume == sound_min(SOUND_VOLUME))
                {
                    *intval = 1;
                }
                else if (global_settings.volume == 0)
                {
                    *intval = limit - 1;
                }
                else if (global_settings.volume > 0)
                {
                    *intval = limit;
                }
                else
                {
                    *intval = (limit - 3) * (global_settings.volume
                                             - sound_min(SOUND_VOLUME) - 1)
                              / (-1 - sound_min(SOUND_VOLUME)) + 2;
                }
            }
            return buf;

        case WPS_TOKEN_TRACK_ELAPSED_PERCENT:
            if (id3->length <= 0)
                return NULL;

            if (intval)
            {
                *intval = limit * (id3->elapsed + state->ff_rewind_count)
                          / id3->length + 1;
            }
            snprintf(buf, buf_size, "%d",
                     100*(id3->elapsed + state->ff_rewind_count) / id3->length);
            return buf;

        case WPS_TOKEN_METADATA_ARTIST:
            return id3->artist;

        case WPS_TOKEN_METADATA_COMPOSER:
            return id3->composer;

        case WPS_TOKEN_METADATA_ALBUM:
            return id3->album;

        case WPS_TOKEN_METADATA_ALBUM_ARTIST:
            return id3->albumartist;

        case WPS_TOKEN_METADATA_GROUPING:
            return id3->grouping;

        case WPS_TOKEN_METADATA_GENRE:
            return id3->genre_string;

        case WPS_TOKEN_METADATA_DISC_NUMBER:
            if (id3->disc_string)
                return id3->disc_string;
            if (id3->discnum) {
                snprintf(buf, buf_size, "%d", id3->discnum);
                return buf;
            }
            return NULL;

        case WPS_TOKEN_METADATA_TRACK_NUMBER:
            if (id3->track_string)
                return id3->track_string;

            if (id3->tracknum) {
                snprintf(buf, buf_size, "%d", id3->tracknum);
                return buf;
            }
            return NULL;

        case WPS_TOKEN_METADATA_TRACK_TITLE:
            return id3->title;

        case WPS_TOKEN_METADATA_VERSION:
            switch (id3->id3version)
            {
                case ID3_VER_1_0:
                    return "1";

                case ID3_VER_1_1:
                    return "1.1";

                case ID3_VER_2_2:
                    return "2.2";

                case ID3_VER_2_3:
                    return "2.3";

                case ID3_VER_2_4:
                    return "2.4";

                default:
                    return NULL;
            }

        case WPS_TOKEN_METADATA_YEAR:
            if( id3->year_string )
                return id3->year_string;

            if (id3->year) {
                snprintf(buf, buf_size, "%d", id3->year);
                return buf;
            }
            return NULL;

        case WPS_TOKEN_METADATA_COMMENT:
            return id3->comment;

#ifdef HAVE_ALBUMART
        case WPS_TOKEN_ALBUMART_DISPLAY:
            draw_album_art(gwps, audio_current_aa_hid(), false);
            return NULL;

        case WPS_TOKEN_ALBUMART_FOUND:
            if (audio_current_aa_hid() >= 0) {
                return "C";
            }
            return NULL;
#endif

        case WPS_TOKEN_FILE_BITRATE:
            if(id3->bitrate)
                snprintf(buf, buf_size, "%d", id3->bitrate);
            else
                return "?";
            return buf;

        case WPS_TOKEN_FILE_CODEC:
            if (intval)
            {
                if(id3->codectype == AFMT_UNKNOWN)
                    *intval = AFMT_NUM_CODECS;
                else
                    *intval = id3->codectype;
            }
            return get_codectype(id3);

        case WPS_TOKEN_FILE_FREQUENCY:
            snprintf(buf, buf_size, "%ld", id3->frequency);
            return buf;

        case WPS_TOKEN_FILE_FREQUENCY_KHZ:
            /* ignore remainders < 100, so 22050 Hz becomes just 22k */
            if ((id3->frequency % 1000) < 100)
                snprintf(buf, buf_size, "%ld", id3->frequency / 1000);
            else
                snprintf(buf, buf_size, "%ld.%d",
                        id3->frequency / 1000,
                        (id3->frequency % 1000) / 100);
            return buf;

        case WPS_TOKEN_FILE_NAME:
            if (get_dir(buf, buf_size, id3->path, 0)) {
                /* Remove extension */
                char* sep = strrchr(buf, '.');
                if (NULL != sep) {
                    *sep = 0;
                }
                return buf;
            }
            else {
                return NULL;
            }

        case WPS_TOKEN_FILE_NAME_WITH_EXTENSION:
            return get_dir(buf, buf_size, id3->path, 0);

        case WPS_TOKEN_FILE_PATH:
            return id3->path;

        case WPS_TOKEN_FILE_SIZE:
            snprintf(buf, buf_size, "%ld", id3->filesize / 1024);
            return buf;

        case WPS_TOKEN_FILE_VBR:
            return id3->vbr ? "(avg)" : NULL;

        case WPS_TOKEN_FILE_DIRECTORY:
            return get_dir(buf, buf_size, id3->path, token->value.i);

        case WPS_TOKEN_BATTERY_PERCENT:
        {
            int l = battery_level();

            if (intval)
            {
                limit = MAX(limit, 2);
                if (l > -1) {
                    /* First enum is used for "unknown level". */
                    *intval = (limit - 1) * l / 100 + 2;
                } else {
                    *intval = 1;
                }
            }

            if (l > -1) {
                snprintf(buf, buf_size, "%d", l);
                return buf;
            } else {
                return "?";
            }
        }

        case WPS_TOKEN_BATTERY_VOLTS:
        {
            unsigned int v = battery_voltage();
            snprintf(buf, buf_size, "%d.%02d", v / 1000, (v % 1000) / 10);
            return buf;
        }

        case WPS_TOKEN_BATTERY_TIME:
        {
            int t = battery_time();
            if (t >= 0)
                snprintf(buf, buf_size, "%dh %dm", t / 60, t % 60);
            else
                return "?h ?m";
            return buf;
        }

#if CONFIG_CHARGING
        case WPS_TOKEN_BATTERY_CHARGER_CONNECTED:
        {
            if(charger_input_state==CHARGER)
                return "p";
            else
                return NULL;
        }
#endif
#if CONFIG_CHARGING >= CHARGING_MONITOR
        case WPS_TOKEN_BATTERY_CHARGING:
        {
            if (charge_state == CHARGING || charge_state == TOPOFF) {
                return "c";
            } else {
                return NULL;
            }
        }
#endif
        case WPS_TOKEN_BATTERY_SLEEPTIME:
        {
            if (get_sleep_timer() == 0)
                return NULL;
            else
            {
                format_time(buf, buf_size, get_sleep_timer() * 1000);
                return buf;
            }
        }

        case WPS_TOKEN_PLAYBACK_STATUS:
        {
            int status = audio_status();
            int mode = 1;
            if (status == AUDIO_STATUS_PLAY)
                mode = 2;
            if (is_wps_fading() || 
               (status & AUDIO_STATUS_PAUSE && !status_get_ffmode()))
                mode = 3;
            if (status_get_ffmode() == STATUS_FASTFORWARD)
                mode = 4;
            if (status_get_ffmode() == STATUS_FASTBACKWARD)
                mode = 5;

            if (intval) {
                *intval = mode;
            }

            snprintf(buf, buf_size, "%d", mode-1);
            return buf;
        }

        case WPS_TOKEN_REPEAT_MODE:
            if (intval)
                *intval = global_settings.repeat_mode + 1;
            snprintf(buf, buf_size, "%d", global_settings.repeat_mode);
            return buf;
            
        case WPS_TOKEN_RTC_PRESENT:
#if CONFIG_RTC
                return "c";
#else
                return NULL;
#endif

#if CONFIG_RTC
        case WPS_TOKEN_RTC_12HOUR_CFG:
            if (intval)
                *intval = global_settings.timeformat + 1;
            snprintf(buf, buf_size, "%d", global_settings.timeformat);
            return buf;

        case WPS_TOKEN_RTC_DAY_OF_MONTH:
            /* d: day of month (01..31) */
            snprintf(buf, buf_size, "%02d", tm->tm_mday);
            return buf;

        case WPS_TOKEN_RTC_DAY_OF_MONTH_BLANK_PADDED:
            /* e: day of month, blank padded ( 1..31) */
            snprintf(buf, buf_size, "%2d", tm->tm_mday);
            return buf;

        case WPS_TOKEN_RTC_HOUR_24_ZERO_PADDED:
            /* H: hour (00..23) */
            snprintf(buf, buf_size, "%02d", tm->tm_hour);
            return buf;

        case WPS_TOKEN_RTC_HOUR_24:
            /* k: hour ( 0..23) */
            snprintf(buf, buf_size, "%2d", tm->tm_hour);
            return buf;

        case WPS_TOKEN_RTC_HOUR_12_ZERO_PADDED:
            /* I: hour (01..12) */
            snprintf(buf, buf_size, "%02d",
                     (tm->tm_hour % 12 == 0) ? 12 : tm->tm_hour % 12);
            return buf;

        case WPS_TOKEN_RTC_HOUR_12:
            /* l: hour ( 1..12) */
            snprintf(buf, buf_size, "%2d",
                     (tm->tm_hour % 12 == 0) ? 12 : tm->tm_hour % 12);
            return buf;

        case WPS_TOKEN_RTC_MONTH:
            /* m: month (01..12) */
            if (intval)
                *intval = tm->tm_mon + 1;
            snprintf(buf, buf_size, "%02d", tm->tm_mon + 1);
            return buf;

        case WPS_TOKEN_RTC_MINUTE:
            /* M: minute (00..59) */
            snprintf(buf, buf_size, "%02d", tm->tm_min);
            return buf;

        case WPS_TOKEN_RTC_SECOND:
            /* S: second (00..59) */
            snprintf(buf, buf_size, "%02d", tm->tm_sec);
            return buf;

        case WPS_TOKEN_RTC_YEAR_2_DIGITS:
            /* y: last two digits of year (00..99) */
            snprintf(buf, buf_size, "%02d", tm->tm_year % 100);
            return buf;

        case WPS_TOKEN_RTC_YEAR_4_DIGITS:
            /* Y: year (1970...) */
            snprintf(buf, buf_size, "%04d", tm->tm_year + 1900);
            return buf;

        case WPS_TOKEN_RTC_AM_PM_UPPER:
            /* p: upper case AM or PM indicator */
            return tm->tm_hour/12 == 0 ? "AM" : "PM";

        case WPS_TOKEN_RTC_AM_PM_LOWER:
            /* P: lower case am or pm indicator */
            return tm->tm_hour/12 == 0 ? "am" : "pm";

        case WPS_TOKEN_RTC_WEEKDAY_NAME:
            /* a: abbreviated weekday name (Sun..Sat) */
            return str(LANG_WEEKDAY_SUNDAY + tm->tm_wday);

        case WPS_TOKEN_RTC_MONTH_NAME:
            /* b: abbreviated month name (Jan..Dec) */
            return str(LANG_MONTH_JANUARY + tm->tm_mon);

        case WPS_TOKEN_RTC_DAY_OF_WEEK_START_MON:
            /* u: day of week (1..7); 1 is Monday */
            if (intval)
                *intval = (tm->tm_wday == 0) ? 7 : tm->tm_wday;
            snprintf(buf, buf_size, "%1d", tm->tm_wday + 1);
            return buf;

        case WPS_TOKEN_RTC_DAY_OF_WEEK_START_SUN:
            /* w: day of week (0..6); 0 is Sunday */
            if (intval)
                *intval = tm->tm_wday + 1;
            snprintf(buf, buf_size, "%1d", tm->tm_wday);
            return buf;
#else
        case WPS_TOKEN_RTC_DAY_OF_MONTH:
        case WPS_TOKEN_RTC_DAY_OF_MONTH_BLANK_PADDED:
        case WPS_TOKEN_RTC_HOUR_24_ZERO_PADDED:
        case WPS_TOKEN_RTC_HOUR_24:
        case WPS_TOKEN_RTC_HOUR_12_ZERO_PADDED:
        case WPS_TOKEN_RTC_HOUR_12:
        case WPS_TOKEN_RTC_MONTH:
        case WPS_TOKEN_RTC_MINUTE:
        case WPS_TOKEN_RTC_SECOND:
        case WPS_TOKEN_RTC_AM_PM_UPPER:
        case WPS_TOKEN_RTC_AM_PM_LOWER:
        case WPS_TOKEN_RTC_YEAR_2_DIGITS:
            return "--";
        case WPS_TOKEN_RTC_YEAR_4_DIGITS:
            return "----";
        case WPS_TOKEN_RTC_WEEKDAY_NAME:
        case WPS_TOKEN_RTC_MONTH_NAME:
            return "---";
        case WPS_TOKEN_RTC_DAY_OF_WEEK_START_MON:
        case WPS_TOKEN_RTC_DAY_OF_WEEK_START_SUN:
            return "-";
#endif

#ifdef HAVE_LCD_CHARCELLS
        case WPS_TOKEN_PROGRESSBAR:
        {
            char *end = utf8encode(data->wps_progress_pat[0], buf);
            *end = '\0';
            return buf;
        }

        case WPS_TOKEN_PLAYER_PROGRESSBAR:
            if(is_new_player())
            {
                /* we need 11 characters (full line) for
                    progress-bar */
                strlcpy(buf, "           ", buf_size);
            }
            else
            {
                /* Tell the user if we have an OldPlayer */
                strlcpy(buf, " <Old LCD> ", buf_size);
            }
            return buf;
#endif

#ifdef HAVE_TAGCACHE
        case WPS_TOKEN_DATABASE_PLAYCOUNT:
            if (intval) {
                *intval = id3->playcount + 1;
            }
            snprintf(buf, buf_size, "%ld", id3->playcount);
            return buf;

        case WPS_TOKEN_DATABASE_RATING:
            if (intval) {
                *intval = id3->rating + 1;
            }
            snprintf(buf, buf_size, "%d", id3->rating);
            return buf;

        case WPS_TOKEN_DATABASE_AUTOSCORE:
            if (intval)
                *intval = id3->score + 1;

            snprintf(buf, buf_size, "%d", id3->score);
            return buf;
#endif

#if (CONFIG_CODEC == SWCODEC)
        case WPS_TOKEN_CROSSFADE:
            if (intval)
                *intval = global_settings.crossfade + 1;
            snprintf(buf, buf_size, "%d", global_settings.crossfade);
            return buf;

        case WPS_TOKEN_REPLAYGAIN:
        {
            int val;

            if (global_settings.replaygain_type == REPLAYGAIN_OFF)
                val = 1; /* off */
            else
            {
                int type =
                    get_replaygain_mode(id3->track_gain_string != NULL,
                                        id3->album_gain_string != NULL);
                if (type < 0)
                    val = 6;    /* no tag */
                else
                    val = type + 2;

                if (global_settings.replaygain_type == REPLAYGAIN_SHUFFLE)
                    val += 2;
            }

            if (intval)
                *intval = val;

            switch (val)
            {
                case 1:
                case 6:
                    return "+0.00 dB";
                    break;
                case 2:
                case 4:
                    strlcpy(buf, id3->track_gain_string, buf_size);
                    break;
                case 3:
                case 5:
                    strlcpy(buf, id3->album_gain_string, buf_size);
                    break;
            }
            return buf;
        }
#endif  /* (CONFIG_CODEC == SWCODEC) */

#if (CONFIG_CODEC != MAS3507D)
        case WPS_TOKEN_SOUND_PITCH:
        {
            int val = sound_get_pitch();
            snprintf(buf, buf_size, "%d.%d",
                    val / 10, val % 10);
            return buf;
        }
#endif

        case WPS_TOKEN_MAIN_HOLD:
#ifdef HAS_BUTTON_HOLD
            if (button_hold())
#else
            if (is_keys_locked())
#endif /*hold switch or softlock*/
                return "h";
            else
                return NULL;

#ifdef HAS_REMOTE_BUTTON_HOLD
        case WPS_TOKEN_REMOTE_HOLD:
            if (remote_button_hold())
                return "r";
            else
                return NULL;
#endif

#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
        case WPS_TOKEN_VLED_HDD:
            if(led_read(HZ/2))
                return "h";
            else
                return NULL;
#endif
        case WPS_TOKEN_BUTTON_VOLUME:
            if (data->button_time_volume && 
                TIME_BEFORE(current_tick, data->button_time_volume +
                                          token->value.i * TIMEOUT_UNIT))
                return "v";
            return NULL;
        case WPS_TOKEN_LASTTOUCH:
#ifdef HAVE_TOUCHSCREEN
            if (TIME_BEFORE(current_tick, token->value.i * TIMEOUT_UNIT +
                                          touchscreen_last_touch()))
                return "t";
#endif
            return NULL;

        case WPS_TOKEN_SETTING:
        {
            if (intval)
            {
                /* Handle contionals */
                const struct settings_list *s = settings+token->value.i;
                switch (s->flags&F_T_MASK)
                {
                    case F_T_INT:
                    case F_T_UINT:
                        if (s->flags&F_RGB)
                            /* %?St|name|<#000000|#000001|...|#FFFFFF> */
                            /* shouldn't overflow since colors are stored
                             * on 16 bits ...
                             * but this is pretty useless anyway */
                            *intval = *(int*)s->setting + 1;
                        else if (s->cfg_vals == NULL)
                            /* %?St|name|<1st choice|2nd choice|...> */
                            *intval = (*(int*)s->setting-s->int_setting->min)
                                      /s->int_setting->step + 1;
                        else
                            /* %?St|name|<1st choice|2nd choice|...> */
                            /* Not sure about this one. cfg_name/vals are
                             * indexed from 0 right? */
                            *intval = *(int*)s->setting + 1;
                        break;
                    case F_T_BOOL:
                        /* %?St|name|<if true|if false> */
                        *intval = *(bool*)s->setting?1:2;
                        break;
                    case F_T_CHARPTR:
                        /* %?St|name|<if non empty string|if empty>
                         * The string's emptyness discards the setting's
                         * prefix and suffix */
                        *intval = ((char*)s->setting)[0]?1:2;
                        break;
                    default:
                        /* This shouldn't happen ... but you never know */
                        *intval = -1;
                        break;
                }
            }
            cfg_to_string(token->value.i,buf,buf_size);
            return buf;
        }

        default:
            return NULL;
    }
}
/**
 *  Volnoti - Lightweight Volume Notification
 *  Copyright (C) 2011  David Brazdil <db538@cam.ac.uk>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
 
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <string.h>

#include "common.h"
#include "gopt.h"

#include "value-client-stub.h"

static int get_alsa_volume(int card, int *volume, int *muted) {
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    const char *card_name;
    snd_mixer_elem_t *elem;
    long pmin, pmax, vol;
    int switch_val;
    char card_dev[32];
    
    snprintf(card_dev, sizeof(card_dev), "hw:%d", card);
    
    if (snd_mixer_open(&handle, 0) < 0) {
        return -1;
    }
    
    if (snd_mixer_attach(handle, card_dev) < 0) {
        snd_mixer_close(handle);
        return -1;
    }
    
    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
        snd_mixer_close(handle);
        return -1;
    }
    
    if (snd_mixer_load(handle) < 0) {
        snd_mixer_close(handle);
        return -1;
    }
    
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    
    elem = snd_mixer_find_selem(handle, sid);
    if (!elem) {
        snd_mixer_close(handle);
        return -1;
    }
    
    snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO, &vol);
    
    *volume = (int)((vol * 100) / (pmax - pmin));
    
    if (snd_mixer_selem_has_playback_switch(elem)) {
        snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &switch_val);
        *muted = !switch_val;
    } else {
        *muted = 0;
    }
    
    snd_mixer_close(handle);
    return 0;
}

static void print_usage(const char* filename, int failure) {
    g_print("Usage: %s [-v] [-m] [-a] [-c <card>] [<volume>]\n"
            " -h\t\t--help\t\t\thelp\n"
            " -v\t\t--verbose\t\tverbose\n"
            " -m\t\t--mute\t\t\tmuted\n"
            " -a\t\t--auto\t\t\tauto-detect current volume\n"
            " -c <card>\t--card <card>\t\tALSA card number (default: 0)\n"
            " <volume>\t\t\t\tint 0-100 (not needed with -a)\n", filename);
    if (failure)
        exit(EXIT_FAILURE);
    else
        exit(EXIT_SUCCESS);
}

int main(int argc, const char* argv[]) {
    void *options = gopt_sort(&argc, argv, gopt_start(
            gopt_option('h', 0, gopt_shorts('h', '?'), gopt_longs("help", "HELP")),
            gopt_option('m', 0, gopt_shorts('m'), gopt_longs("mute")),
            gopt_option('a', 0, gopt_shorts('a'), gopt_longs("auto")),
            gopt_option('c', GOPT_ARG, gopt_shorts('c'), gopt_longs("card")),
            gopt_option('v', GOPT_REPEAT, gopt_shorts('v'), gopt_longs("verbose"))));
    int help = gopt(options, 'h');
    int debug = gopt(options, 'v');
    int muted = gopt(options, 'm');
    int auto_detect = gopt(options, 'a');
    int card = 0;
    
    if (gopt(options, 'c')) {
        if (sscanf(gopt_arg_i(options, 'c', 0), "%d", &card) != 1) {
            gopt_free(options);
            print_usage(argv[0], TRUE);
        }
    }
    
    gopt_free(options);

    if (help)
        print_usage(argv[0], FALSE);

    gint volume;
    int alsa_muted = 0;
    
    if (auto_detect) {
        // Auto-detect volume from ALSA
        if (get_alsa_volume(card, &volume, &alsa_muted) < 0) {
            g_print("Error: Failed to get volume from ALSA card %d\n", card);
            return EXIT_FAILURE;
        }
        // Override muted flag if ALSA reports muted
        if (alsa_muted) {
            muted = TRUE;
        }
        if (debug) {
            g_print("Auto-detected: Volume=%d%%, Muted=%s, Card=%d\n", 
                    volume, muted ? "yes" : "no", card);
        }
    } else {
        // Manual volume specification (original behavior)
        if (muted) {
            if (argc > 2) {
                print_usage(argv[0], TRUE);
            } else if (argc == 2) {
                if (sscanf(argv[1], "%d", &volume) != 1)
                    print_usage(argv[0], TRUE);

                if (volume > 100 || volume < 0)
                    print_usage(argv[0], TRUE);
            } else {
                volume = 0;
            }
        } else {
            if (argc != 2)
                print_usage(argv[0], TRUE);

            if (sscanf(argv[1], "%d", &volume) != 1)
                print_usage(argv[0], TRUE);

            if (volume > 100 || volume < 0)
                print_usage(argv[0], TRUE);
        }
    }

    DBusGConnection *bus = NULL;
    DBusGProxy *proxy = NULL;
    GError *error = NULL;

    // initialize GObject
    g_type_init();

    // connect to D-Bus
    print_debug("Connecting to D-Bus...", debug);
    bus = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
    if (error != NULL)
        handle_error("Couldn't connect to D-Bus",
                    error->message,
                    TRUE);
    print_debug_ok(debug);

    // get the proxy
    print_debug("Getting proxy...", debug);
    proxy = dbus_g_proxy_new_for_name(bus,
                                      VALUE_SERVICE_NAME,
                                      VALUE_SERVICE_OBJECT_PATH,
                                      VALUE_SERVICE_INTERFACE);
    if (proxy == NULL)
        handle_error("Couldn't get a proxy for D-Bus",
                    "Unknown(dbus_g_proxy_new_for_name)",
                    TRUE);
    print_debug_ok(debug);

    print_debug("Sending volume...", debug);
    uk_ac_cam_db538_VolumeNotification_notify(proxy, volume, muted, &error);
    if (error !=  NULL) {
        handle_error("Failed to send notification", error->message, FALSE);
        g_clear_error(&error);
        return EXIT_FAILURE;
    }
    print_debug_ok(debug);

    return EXIT_SUCCESS;
}

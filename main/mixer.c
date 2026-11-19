/* mixer.c - the DOS PC's sound (LINE IN) on the board's output
 *
 * Only on boards with a mixer (g_board.mixer). The driver maps the web UI
 * level 0..100 % to its own gain steps. On a codec with two input pairs,
 * the web UI also selects the pair that carries LINE IN.
 */
#include "leadin.h"

static int input_pair;                  /* the pair in use, 0 = the mixer has one input */

bool mixer_available(void)
{
    return g_board.mixer && g_board.mixer->available();
}

int mixer_input(void)
{
    return mixer_available() ? input_pair : 0;
}

void mixer_apply(void)
{
    if (!mixer_available())
        return;
    if (g_board.mixer->input) {
        int pair = g_board.mixer->input(g_set.pc_input);
        if (pair != input_pair)
            nlog("PC sound: LINE IN on input pair %d", pair);
        input_pair = pair;
    }
    g_board.mixer->set(g_set.pc_mute ? 0 : g_set.pc_volume);
}

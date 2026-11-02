/* audio_backend.c — pick the engine's audio backend.
 *
 * The JACK backend is linked in. */
#include <stdio.h>

#include "audio_backend.h"

const JdBackend *jackdaw_backend_entry(uint32_t abi); /* src/backend/jack */

const JdBackend *audio_backend_load(char *err, size_t err_len)
{
    const JdBackend *b = jackdaw_backend_entry(JD_BACKEND_ABI);
    if (!b && err && err_len)
        snprintf(err, err_len, "The JACK backend does not match this JackDAW.");
    return b;
}

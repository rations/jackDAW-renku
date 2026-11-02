/* rt_ringbuffer.c — see rt_ringbuffer.h for why this exists and its ordering.
 *
 * Indices are byte offsets in [0, size). read_ptr == write_ptr means empty;
 * the writer stops one byte short of the reader, so full is never ambiguous
 * with empty. That is the jack_ringbuffer rule, kept so buffers sized for it
 * hold exactly what they held before. */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "rt_ringbuffer.h"

struct rt_ringbuffer {
    char *buf;
    size_t write_ptr; /* stored only by the producer */
    size_t read_ptr;  /* stored only by the consumer */
    size_t size;      /* power of two */
    size_t size_mask;
    int mlocked;
};

/* The other side's index: acquire, so its copy is visible before we act on
 * the space it reports. */
static inline size_t rb_load_acq(const size_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* Our own index, after our copy: release, so the copy is visible before the
 * other side can see the index move. */
static inline void rb_store_rel(size_t *p, size_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

rt_ringbuffer_t *rt_ringbuffer_create(size_t sz)
{
    rt_ringbuffer_t *rb = malloc(sizeof *rb);
    if (!rb)
        return NULL;

    size_t size = 2;
    while (size < sz) {
        if (size > ((size_t)-1 >> 1)) {
            free(rb);
            return NULL;
        }
        size <<= 1;
    }

    rb->buf = malloc(size);
    if (!rb->buf) {
        free(rb);
        return NULL;
    }
    rb->size = size;
    rb->size_mask = size - 1;
    rb->write_ptr = 0;
    rb->read_ptr = 0;
    rb->mlocked = 0;
    return rb;
}

void rt_ringbuffer_free(rt_ringbuffer_t *rb)
{
    if (!rb)
        return;
    if (rb->mlocked)
        munlock(rb->buf, rb->size);
    free(rb->buf);
    free(rb);
}

/* 0 on success, -1 if mlock failed (typically RLIMIT_MEMLOCK); the ring works
 * either way, it just may page. */
int rt_ringbuffer_mlock(rt_ringbuffer_t *rb)
{
    if (mlock(rb->buf, rb->size))
        return -1;
    rb->mlocked = 1;
    return 0;
}

/* Not thread safe: only while neither side can be touching the ring. */
void rt_ringbuffer_reset(rt_ringbuffer_t *rb)
{
    rb_store_rel(&rb->read_ptr, 0);
    rb_store_rel(&rb->write_ptr, 0);
}

size_t rt_ringbuffer_read_space(const rt_ringbuffer_t *rb)
{
    size_t w = rb_load_acq(&rb->write_ptr);
    size_t r = rb_load_acq(&rb->read_ptr);
    return (w - r) & rb->size_mask;
}

size_t rt_ringbuffer_write_space(const rt_ringbuffer_t *rb)
{
    size_t w = rb_load_acq(&rb->write_ptr);
    size_t r = rb_load_acq(&rb->read_ptr);
    return (r - w - 1) & rb->size_mask;
}

/* Copy up to cnt bytes out from the read index without moving it. */
static size_t rb_copy_out(const rt_ringbuffer_t *rb, char *dest, size_t cnt)
{
    size_t avail = rt_ringbuffer_read_space(rb);
    size_t n = cnt < avail ? cnt : avail;
    if (n == 0)
        return 0;

    size_t r = rb->read_ptr;
    size_t n1 = rb->size - r;
    if (n1 > n)
        n1 = n;
    memcpy(dest, rb->buf + r, n1);
    if (n > n1)
        memcpy(dest + n1, rb->buf, n - n1);
    return n;
}

size_t rt_ringbuffer_peek(rt_ringbuffer_t *rb, char *dest, size_t cnt)
{
    return rb_copy_out(rb, dest, cnt);
}

size_t rt_ringbuffer_read(rt_ringbuffer_t *rb, char *dest, size_t cnt)
{
    size_t n = rb_copy_out(rb, dest, cnt);
    if (n)
        rb_store_rel(&rb->read_ptr, (rb->read_ptr + n) & rb->size_mask);
    return n;
}

/* Discard up to cnt readable bytes. Clamped to what is readable, so a caller
 * that skips a malformed record cannot move the reader past the writer. */
void rt_ringbuffer_read_advance(rt_ringbuffer_t *rb, size_t cnt)
{
    size_t avail = rt_ringbuffer_read_space(rb);
    size_t n = cnt < avail ? cnt : avail;
    rb_store_rel(&rb->read_ptr, (rb->read_ptr + n) & rb->size_mask);
}

size_t rt_ringbuffer_write(rt_ringbuffer_t *rb, const char *src, size_t cnt)
{
    size_t space = rt_ringbuffer_write_space(rb);
    size_t n = cnt < space ? cnt : space;
    if (n == 0)
        return 0;

    size_t w = rb->write_ptr;
    size_t n1 = rb->size - w;
    if (n1 > n)
        n1 = n;
    memcpy(rb->buf + w, src, n1);
    if (n > n1)
        memcpy(rb->buf, src + n1, n - n1);
    rb_store_rel(&rb->write_ptr, (w + n) & rb->size_mask);
    return n;
}

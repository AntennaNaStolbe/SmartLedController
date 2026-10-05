#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Wall-clock time obtained from the Zigbee Time cluster (0x000A).
 *
 * The cluster delivers UTC, so a local UTC offset is applied on top. When the
 * coordinator publishes a TimeZone attribute it wins over the compiled-in
 * fallback in config.h.
 */

/* Start the periodic resync timer. Call after the Zigbee stack is up. */
void clock_init(void);

/* Called by the Zigbee layer whenever a Time / TimeZone attribute arrives. */
void clock_set_zcl_time(uint32_t zcl_time_s);
void clock_set_timezone(int32_t offset_s);
void clock_clear_timezone(void);

bool clock_is_synced(void);

/* "HH:MM" and "DD.MM.YYYY" into caller-provided buffers. */
void clock_get_time_str(char *buf, unsigned len);
void clock_get_date_str(char *buf, unsigned len);

/* Called by clock_init on a timer so the Zigbee layer can re-read the Time
 * attribute; wired up from zb_app.c. */
typedef void (*clock_resync_cb_t)(void);
void clock_set_resync_cb(clock_resync_cb_t cb);

#endif /* CLOCK_H */
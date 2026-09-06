"""Execute the production LED owner against delayed work and GPIO failures."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BOARD = (ROOT / "app/src/app_board.c").read_text()
ACTIONS = (ROOT / "app/src/app_anchor_actions.c").read_text()


def function(source, signature):
    start = source.index(signature + "\n{")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


native = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define ANCHOR_IDENTIFY_DURATION_MS 10000u
#define ROLE_ANCHOR 2
#define DEVICE_ROLE 2
#define DT_NODE_HAS_STATUS(a,b) 1
#define ARG_UNUSED(a) (void)(a)
#define K_FOREVER 0
#define K_MSEC(a) (a)
#define MIN(a,b) ((a)<(b)?(a):(b))
struct gpio_dt_spec { unsigned pin; };
static const struct gpio_dt_spec status0_red={0}, status0_green={1}, status0_blue={2};
static const struct gpio_dt_spec status1_red={3}, status1_green={4}, status1_blue={5};
struct k_work { int unused; };
struct k_work_delayable { int unused; };
static struct k_work_delayable status0_identify_work;
static uint64_t status0_identify_started_ms, now;
static uint8_t status0_desired_rgb;
static bool status0_identify_active, status0_disconnected;
static bool status0_identify_ready=true;
static int status0_mutex, schedule_error, configure_error;
static uint32_t scheduled;
static bool lit[6], connected[6];
static void status0_disconnect(void);
static void k_mutex_lock(int *m, int delay) { (void)delay; assert(*m==0); *m=1; }
static void k_mutex_unlock(int *m) { assert(*m==1); *m=0; }
static int64_t k_uptime_get(void) { return (int64_t)now; }
static void status_debug_printf(const char *fmt, ...) { (void)fmt; }
static int k_work_reschedule(struct k_work_delayable *w, uint32_t delay)
{ assert(w==&status0_identify_work); assert(status0_mutex==1); scheduled=delay; return schedule_error; }
static int configure_output(const struct gpio_dt_spec *gpio)
{ connected[gpio->pin]=true; lit[gpio->pin]=false; return configure_error; }
static void set_output(const struct gpio_dt_spec *gpio, bool value)
{ assert(status0_mutex==1); lit[gpio->pin]=value; }
static void disconnect_gpio(const struct gpio_dt_spec *gpio)
{ connected[gpio->pin]=false; lit[gpio->pin]=false; }
'''

native += function(ACTIONS, "uint8_t app_anchor_identify_color(uint32_t elapsed_ms, uint32_t *next_ms)")
for signature in (
    "static void status_led0_apply(bool red, bool green, bool blue)",
    "void status_led0_set(bool red, bool green, bool blue)",
    "static void status0_identify_finish(void)",
    "static void status0_identify_work_handler(struct k_work *work)",
    "int status_identify_anchor(void)",
    "static void status0_disconnect(void)",
    "void status_leds_disconnect(void)",
):
    native += function(BOARD, signature)

native += r'''
static unsigned rgb(void) { return lit[0] | (lit[1]<<1) | (lit[2]<<2); }
int main(void)
{
    now=UINT64_C(0xffffffff)-3000;
    assert(status_identify_anchor()==0 && rgb()==1 && scheduled==250);
    uint64_t started=status0_identify_started_ms;
    unsigned seen=0;
    while(now < started+10000) {
        assert(scheduled>0 && now+scheduled<=started+10000);
        seen |= rgb();
        /* Unrelated normal status updates cannot steal an active override. */
        unsigned old=rgb(); status_led0_set(true,true,true); assert(rgb()==old);
        now += scheduled;
        status0_identify_work_handler(NULL);
    }
    assert(seen==7 && !status0_identify_active && rgb()==7);
    /* Low power parking waits for the override, then parks the actual pins. */
    assert(status_identify_anchor()==0);
    started=now;
    status_leds_disconnect();
    assert(connected[0] && connected[1] && connected[2] && rgb()==1);
    now=started+9999;
    status0_identify_work_handler(NULL);
    assert(scheduled==1 && status0_identify_active);
    now=started+10013; /* A late callback expires, never restarts its phase. */
    status0_identify_work_handler(NULL);
    assert(!status0_identify_active && rgb()==0);
    assert(!connected[0] && !connected[1] && !connected[2]);
    status0_disconnected=false;
    status_led0_set(false,true,false);
    schedule_error=-EIO;
    assert(status_identify_anchor()==-EIO && !status0_identify_active && rgb()==2);
    schedule_error=0;
    assert(status_identify_anchor()==0);
    now+=250; schedule_error=-EIO;
    status0_identify_work_handler(NULL);
    assert(!status0_identify_active && rgb()==2);
    schedule_error=0; configure_error=-ENODEV;
    assert(status_identify_anchor()==-ENODEV && !status0_identify_active && rgb()==2);
    assert(status0_mutex==0);
    puts("production RGB owner: absolute expiry, normal writes, parking and failures passed");
}
'''

with tempfile.TemporaryDirectory(prefix="anchor-identify-owner-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(native)
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

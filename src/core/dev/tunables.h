#ifndef __CORE_DEV_TUNABLES_H__
#define __CORE_DEV_TUNABLES_H__

// Developer-only RAM tunables (task 1, phase 1).
//
// One table describes every number that used to be a magic constant, so it can
// be read and changed at runtime (`tune` over the console, or the Tunables page
// under Config > System Config > Developer Options) instead of costing a
// 15-25 minute rebuild.
//
// Gating: the *verbs* and the menu page are developer-only and refuse with
// "ERR: developer mode off" when the switch is off. The VALUES are not gated -
// they live in bruce.conf and still apply to a stock build, because a tuning
// that silently changed back when dev mode was turned off would be worse than
// no tuning at all.

#include <stdint.h>

enum tune_type : uint8_t { TUNE_U8 = 0, TUNE_U16, TUNE_U32, TUNE_I32, TUNE_BOOL };

// When the value takes effect.
#define TUNE_LIVE 0x01   // applied to the running loops immediately
#define TUNE_REBOOT 0x02 // persisted; `tune set` reports "pending reboot"

// Where the value lives / whether anything reads it yet.
#define TUNE_BACKED 0x04  // value is owned by its own bruce.conf key (this table
                          // only exposes it); `tune save` leaves it alone
#define TUNE_PENDING 0x08 // no consumer wired yet: stored and reported as
                          // "stored only", never pretend it does something
// A variant table (task 1, phase 2): the value is an index into a small fixed
// set of precompiled behaviours. The behaviour lives in the consumer; the table
// only names the choices, so the selection stays one normal tunable with one
// bruce.conf key and `patch` can spell the names out.
#define TUNE_ENUM 0x10

struct tune_entry {
    const char *name;
    enum tune_type type; // u8, u16, u32, i32, bool
    void *ptr;
    int32_t min, max, def;
    uint8_t flags; // TUNE_LIVE, TUNE_REBOOT, TUNE_BACKED, TUNE_PENDING, TUNE_ENUM
    // Null-terminated variant names, one per legal value, for a TUNE_ENUM entry.
    // Scalar entries leave it out (zero-initialized), so the phase 1 entries did
    // not have to change.
    const char *const *variants;
};

// Table access. Order is the order the specs list, and `tune list` prints it.
int tuneCount();
const tune_entry *tuneAt(int i);
const tune_entry *tuneFind(const char *name);
const char *tuneTypeName(uint8_t type);

// Read / write one entry. tuneSet clamps into [min,max] and reports whether it
// had to, because a silently clamped value is a lie the caller cannot see.
int32_t tuneGet(const tune_entry *e);
int32_t tuneSet(tune_entry *e, int32_t value, bool *clamped);
void tuneReset(tune_entry *e);
void tuneResetAll();

// Name-based helpers for bruce.conf: the whole set is validated on load and a
// single bad entry falls back to its own default instead of failing the boot.
bool tuneSetByName(const char *name, int32_t value);
int32_t tuneGetByName(const char *name, bool *found);

// Variant helpers. All three answer 0 / nullptr for a scalar entry.
int tuneVariantCount(const tune_entry *e);
const char *tuneVariantNameAt(const tune_entry *e, int32_t index);
const char *tuneVariantName(const tune_entry *e); // name the current value selects

#endif

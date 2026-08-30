// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "d_event.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "info.h"
#include "p_local.h"
#include "p_tick.h"
#include "tables.h"

#include "esp_fidget.h"

#define FIDGET_EPISODE 3
#define FIDGET_MAP 9
#define FIDGET_MONSTER_SLOTS 8
#define FIDGET_CORPSE_TICS 50

typedef struct {
    mobj_t *mo;
    unsigned dead_tics;
} fidget_slot_t;

static bool start_requested;
static bool starting;
static bool active;
static unsigned spawn_cursor;
static unsigned spawn_delay;
static unsigned weapon_stage;
static fidget_slot_t slots[FIDGET_MONSTER_SLOTS];

static const weapontype_t stage_weapons[] = {
    wp_pistol, wp_shotgun, wp_chaingun, wp_missile, wp_plasma, wp_bfg,
};

/* Front-load the first reward: the pistol averages roughly one arena kill per
 * ten seconds, while every later weapon accelerates the curve naturally. */
static const unsigned stage_kills[] = { 0, 4, 12, 28, 52, 84 };

static const char *const stage_names[] = {
    "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKET", "PLASMA", "BFG 9000",
};

static const char *const stage_messages[] = {
    "HORDE MODE: PRESS TO FIRE",
    "4 KILLS: SHOTGUN UNLOCKED",
    "12 KILLS: CHAINGUN UNLOCKED",
    "28 KILLS: ROCKET LAUNCHER",
    "52 KILLS: PLASMA RIFLE",
    "84 KILLS: BFG 9000",
};

void esp_fidget_request_start(void)
{
    if (!active && !starting) start_requested = true;
}

bool esp_fidget_engaged(void)
{
    return start_requested || starting || active;
}

bool esp_fidget_active(void)
{
    return active;
}

unsigned esp_fidget_kills(void)
{
    return active ? (unsigned)players[consoleplayer].killcount : 0;
}

const char *esp_fidget_weapon_name(void)
{
    return stage_names[weapon_stage < sizeof(stage_names) / sizeof(stage_names[0])
                       ? weapon_stage : 0];
}

void esp_fidget_pre_game_ticker(void)
{
    if (!start_requested) return;
    start_requested = false;
    starting = true;
    active = false;
    printf("FIDGET: loading custom arena E%dM%d\n", FIDGET_EPISODE,
           FIDGET_MAP);
    G_DeferedInitNew(sk_medium, FIDGET_EPISODE, FIDGET_MAP, false);
}

static unsigned desired_weapon_stage(unsigned kills)
{
    unsigned stage = 0;
    for (unsigned i = 1; i < sizeof(stage_kills) / sizeof(stage_kills[0]); ++i)
        if (kills >= stage_kills[i]) stage = i;
    return stage;
}

static mobjtype_t monster_for_kills(unsigned kills, unsigned sequence)
{
    static const mobjtype_t early[] = { MT_TROOP, MT_TROOP, MT_SERGEANT };
    static const mobjtype_t middle[] = { MT_TROOP, MT_SERGEANT, MT_HEAD };
    static const mobjtype_t hard[] = { MT_SERGEANT, MT_HEAD, MT_BRUISER };
    if (kills < 12) return early[sequence % 3];
    if (kills < 52) return middle[sequence % 3];
    return hard[sequence % 3];
}

static void remove_old_corpses(unsigned *living)
{
    for (unsigned i = 0; i < FIDGET_MONSTER_SLOTS; ++i) {
        fidget_slot_t *slot = &slots[i];
        if (!slot->mo) continue;
        if (slot->mo->thinker.function == ThinkF_REMOVED) {
            slot->mo = NULL;
            slot->dead_tics = 0;
            continue;
        }
        if (mobj_full(slot->mo)->health > 0) {
            ++*living;
            continue;
        }
        if (++slot->dead_tics >= FIDGET_CORPSE_TICS) {
            P_RemoveMobj(slot->mo);
            slot->mo = NULL;
            slot->dead_tics = 0;
        }
    }
}

static void spawn_monster(player_t *player)
{
    /* One exact firing lane makes every shot meaningful without a turn axis.
     * The staggered distances form a visible, collision-safe queue. */
    static const int16_t spawn_x[] = { 420, 340, 260, 180, 100, 460, 380, 300 };
    static const int16_t spawn_y[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned slot_index;
    for (slot_index = 0; slot_index < FIDGET_MONSTER_SLOTS; ++slot_index)
        if (!slots[slot_index].mo) break;
    if (slot_index == FIDGET_MONSTER_SLOTS) return;

    unsigned position = spawn_cursor % FIDGET_MONSTER_SLOTS;
    mobjtype_t type = monster_for_kills((unsigned)player->killcount,
                                        spawn_cursor);
    ++spawn_cursor;
    mobj_t *monster = P_SpawnMobj((fixed_t)spawn_x[position] * FRACUNIT,
                                  (fixed_t)spawn_y[position] * FRACUNIT,
                                  ONFLOORZ, type);
    if (!monster) return;
    if (!P_CheckPosition(monster, monster->xy.x, monster->xy.y)) {
        P_RemoveMobj(monster);
        return;
    }
    mobj_full(monster)->angle = ANG180;
    mobj_full(monster)->reactiontime = 0;
    mobj_full(monster)->threshold = 100;
    mobj_full(monster)->sp_target = mobj_to_shortptr(player->mo);
    P_SetMobjState(monster, mobj_info(monster)->seestate);
    slots[slot_index].mo = monster;
    slots[slot_index].dead_tics = 0;
}

static void initialize_mode(player_t *player)
{
    memset(slots, 0, sizeof(slots));
    spawn_cursor = 0;
    spawn_delay = 1;
    weapon_stage = 0;
    player->killcount = 0;
    player->readyweapon = wp_pistol;
    player->pendingweapon = wp_nochange;
    player->message = stage_messages[0];
    active = true;
    starting = false;
    printf("FIDGET: arena active; one-button fire, bounded infinite horde\n");
}

void esp_fidget_level_ticker(void)
{
    if (starting) {
        if (gamestate != GS_LEVEL || gameepisode != FIDGET_EPISODE ||
            gamemap != FIDGET_MAP || !players[consoleplayer].mo)
            return;
        initialize_mode(&players[consoleplayer]);
    }
    if (!active) return;
    if (gamestate != GS_LEVEL || gameepisode != FIDGET_EPISODE ||
        gamemap != FIDGET_MAP) {
        active = false;
        memset(slots, 0, sizeof(slots));
        return;
    }

    player_t *player = &players[consoleplayer];
    if (!player->mo) return;

    /* This arena is deliberately a one-button game. Keep the authentic attack
     * bit and suppress every source of movement, turning, use, or weapon keys. */
    player->cmd.forwardmove = 0;
    player->cmd.sidemove = 0;
    player->cmd.angleturn = 0;
    player->cmd.buttons &= BT_ATTACK;
    player->cheats |= CF_GODMODE | CF_NOMOMENTUM;
    player->health = 100;
    mobj_full(player->mo)->health = 100;
    mobj_full(player->mo)->momx = 0;
    mobj_full(player->mo)->momy = 0;
    mobj_full(player->mo)->momz = 0;
    mobj_full(player->mo)->angle = 0;

    for (unsigned i = 0; i < NUMWEAPONS; ++i)
        player->weaponowned[i] = i != wp_supershotgun;
    for (unsigned i = 0; i < NUMAMMO; ++i)
        player->ammo[i] = player->maxammo[i];

    unsigned next_stage = desired_weapon_stage((unsigned)player->killcount);
    if (next_stage != weapon_stage) {
        weapon_stage = next_stage;
        player->pendingweapon = stage_weapons[weapon_stage];
        player->message = stage_messages[weapon_stage];
        printf("FIDGET: %u kills, upgrading to %s\n",
               (unsigned)player->killcount, stage_names[weapon_stage]);
    }

    unsigned living = 0;
    remove_old_corpses(&living);
    unsigned target = 3 + (unsigned)player->killcount / 20;
    if (target > 6) target = 6;
    if (spawn_delay) --spawn_delay;
    if (living < target && !spawn_delay) {
        spawn_monster(player);
        spawn_delay = 12;
    }
}

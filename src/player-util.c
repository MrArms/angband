/**
 * \file player-util.c
 * \brief Player utility functions
 *
 * Copyright (c) 2011 The Angband Developers. See COPYING.
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "cave.h"
#include "cmd-core.h"
#include "game-input.h"
#include "game-world.h"
#include "init.h"
#include "obj-gear.h"
#include "obj-identify.h"
#include "obj-tval.h"
#include "obj-pile.h"
#include "obj-util.h"
#include "player-history.h"
#include "player-spell.h"
#include "player-timed.h"
#include "player-util.h"
#include "score.h"
#include "store.h"
#include "target.h"

/**
 * Change dungeon level - e.g. by going up stairs or with WoR.
 */
void dungeon_change_level(int dlev)
{
	/* New depth */
	player->depth = dlev;

	/* If we're returning to town, update the store contents
	   according to how long we've been away */
	if (!dlev && daycount)
		store_update();

	/* Leaving, make new level */
	player->upkeep->generate_level = TRUE;

	/* Save the game when we arrive on the new level. */
	player->upkeep->autosave = TRUE;
}


/**
 * Decreases players hit points and sets death flag if necessary
 *
 * Hack -- this function allows the user to save (or quit) the game
 * when he dies, since the "You die." message is shown before setting
 * the player to "dead".
 */
void take_hit(struct player *p, int dam, const char *kb_str)
{
	int old_chp = p->chp;

	int warning = (p->mhp * op_ptr->hitpoint_warn / 10);

	/* Paranoia */
	if (p->is_dead) return;

	/* Disturb */
	disturb(p, 1);

	/* Mega-Hack -- Apply "invulnerability" */
	if (p->timed[TMD_INVULN] && (dam < 9000)) return;

	/* Hurt the player */
	p->chp -= dam;

	/* Display the hitpoints */
	p->upkeep->redraw |= (PR_HP);

	/* Dead player */
	if (p->chp < 0) {
		/* Allow cheating */
		if ((p->wizard || OPT(cheat_live)) && !get_check("Die? "))
			event_signal(EVENT_CHEAT_DEATH);

		/* Hack -- Note death */
		msgt(MSG_DEATH, "You die.");
		event_signal(EVENT_MESSAGE_FLUSH);

		/* Note cause of death */
		my_strcpy(p->died_from, kb_str, sizeof(p->died_from));

		/* No longer a winner */
		p->total_winner = FALSE;

		/* Note death */
		p->is_dead = TRUE;

		/* Dead */
		return;
	}

	/* Hitpoint warning */
	if (p->chp < warning) {
		/* Hack -- bell on first notice */
		if (old_chp > warning)
			bell("Low hitpoint warning!");

		/* Message */
		msgt(MSG_HITPOINT_WARN, "*** LOW HITPOINT WARNING! ***");
		event_signal(EVENT_MESSAGE_FLUSH);
	}
}

/**
 * Win or not, know inventory, home items and history upon death, enter score
 */
void death_knowledge(void)
{
	struct store *home = &stores[STORE_HOME];
	object_type *obj;
	time_t death_time = (time_t)0;

	/* Retire in the town in a good state */
	if (player->total_winner) {
		player->depth = 0;
		my_strcpy(player->died_from, "Ripe Old Age", sizeof(player->died_from));
		player->exp = player->max_exp;
		player->lev = player->max_lev;
		player->au += 10000000L;
	}

	for (obj = player->gear; obj; obj = obj->next) {
		object_flavor_aware(obj);
		object_notice_everything(obj);
	}

	for (obj = home->stock; obj; obj = obj->next) {
		object_flavor_aware(obj);
		object_notice_everything(obj);
	}

	history_unmask_unknown();

	/* Get time of death */
	(void)time(&death_time);
	enter_score(&death_time);

	/* Hack -- Recalculate bonuses */
	player->upkeep->update |= (PU_BONUS);
	handle_stuff(player->upkeep);
}

/*
 * Modify a stat value by a "modifier", return new value
 *
 * Stats go up: 3,4,...,17,18,18/10,18/20,...,18/220
 * Or even: 18/13, 18/23, 18/33, ..., 18/220
 *
 * Stats go down: 18/220, 18/210,..., 18/10, 18, 17, ..., 3
 * Or even: 18/13, 18/03, 18, 17, ..., 3
 */
s16b modify_stat_value(int value, int amount)
{
	int i;

	/* Reward */
	if (amount > 0)
	{
		/* Apply each point */
		for (i = 0; i < amount; i++)
		{
			/* One point at a time */
			if (value < 18) value++;

			/* Ten "points" at a time */
			else value += 10;
		}
	}

	/* Penalty */
	else if (amount < 0)
	{
		/* Apply each point */
		for (i = 0; i < (0 - amount); i++)
		{
			/* Ten points at a time */
			if (value >= 18+10) value -= 10;

			/* Hack -- prevent weirdness */
			else if (value > 18) value = 18;

			/* One point at a time */
			else if (value > 3) value--;
		}
	}

	/* Return new value */
	return (value);
}

/**
 * Regenerate hit points
 */
void player_regen_hp(void)
{
	s32b new_chp, new_chp_frac;
	int old_chp, percent = 0;

	/* Save the old hitpoints */
	old_chp = player->chp;

	/* Default regeneration */
	if (player->food >= PY_FOOD_WEAK)
		percent = PY_REGEN_NORMAL;
	else if (player->food >= PY_FOOD_FAINT)
		percent = PY_REGEN_WEAK;
	else if (player->food >= PY_FOOD_STARVE)
		percent = PY_REGEN_FAINT;

	/* Various things speed up regeneration */
	if (player_of_has(player, OF_REGEN))
		percent *= 2;
	if (player->searching || player_resting_can_regenerate(player))
		percent *= 2;

	/* Some things slow it down */
	if (player_of_has(player, OF_IMPAIR_HP))
		percent /= 2;

	/* Various things interfere with physical healing */
	if (player->timed[TMD_PARALYZED]) percent = 0;
	if (player->timed[TMD_POISONED]) percent = 0;
	if (player->timed[TMD_STUN]) percent = 0;
	if (player->timed[TMD_CUT]) percent = 0;

	/* Extract the new hitpoints */
	new_chp = ((long)player->mhp) * percent + PY_REGEN_HPBASE;
	player->chp += (s16b)(new_chp >> 16);   /* div 65536 */

	/* check for overflow */
	if ((player->chp < 0) && (old_chp > 0))
		player->chp = MAX_SHORT;
	new_chp_frac = (new_chp & 0xFFFF) + player->chp_frac;	/* mod 65536 */
	if (new_chp_frac >= 0x10000L) {
		player->chp_frac = (u16b)(new_chp_frac - 0x10000L);
		player->chp++;
	} else {
		player->chp_frac = (u16b)new_chp_frac;
	}

	/* Fully healed */
	if (player->chp >= player->mhp) {
		player->chp = player->mhp;
		player->chp_frac = 0;
	}

	/* Notice changes */
	if (old_chp != player->chp) {
		player->upkeep->redraw |= (PR_HP);
		equip_notice_flag(player, OF_REGEN);
		equip_notice_flag(player, OF_IMPAIR_HP);
	}
}


/**
 * Regenerate mana points
 */
void player_regen_mana(void)
{
	s32b new_mana, new_mana_frac;
	int old_csp, percent;

	/* Save the old spell points */
	old_csp = player->csp;

	/* Default regeneration */
	percent = PY_REGEN_NORMAL;

	/* Various things speed up regeneration */
	if (player_of_has(player, OF_REGEN))
		percent *= 2;
	if (player->searching || player_resting_can_regenerate(player))
		percent *= 2;

	/* Some things slow it down */
	if (player_of_has(player, OF_IMPAIR_MANA))
		percent /= 2;

	/* Regenerate mana */
	new_mana = ((long)player->msp) * percent + PY_REGEN_MNBASE;
	player->csp += (s16b)(new_mana >> 16);	/* div 65536 */

	/* check for overflow */
	if ((player->csp < 0) && (old_csp > 0)) {
		player->csp = MAX_SHORT;
	}
	new_mana_frac = (new_mana & 0xFFFF) + player->csp_frac;	/* mod 65536 */
	if (new_mana_frac >= 0x10000L) {
		player->csp_frac = (u16b)(new_mana_frac - 0x10000L);
		player->csp++;
	} else {
		player->csp_frac = (u16b)new_mana_frac;
	}

	/* Must set frac to zero even if equal */
	if (player->csp >= player->msp) {
		player->csp = player->msp;
		player->csp_frac = 0;
	}

	/* Notice changes */
	if (old_csp != player->csp) {
		player->upkeep->redraw |= (PR_MANA);
		equip_notice_flag(player, OF_REGEN);
		equip_notice_flag(player, OF_IMPAIR_MANA);
	}
}

/**
 * Update the player's light fuel
 */
void player_update_light(void)
{
	/* Check for light being wielded */
	struct object *obj = equipped_item_by_slot_name(player, "light");

	/* Burn some fuel in the current light */
	if (obj && tval_is_light(obj)) {
		bool burn_fuel = TRUE;

		/* Turn off the wanton burning of light during the day in the town */
		if (!player->depth && is_daytime())
			burn_fuel = FALSE;

		/* If the light has the NO_FUEL flag, well... */
		if (of_has(obj->flags, OF_NO_FUEL))
		    burn_fuel = FALSE;

		/* Use some fuel (except on artifacts, or during the day) */
		if (burn_fuel && obj->timeout > 0) {
			/* Decrease life-span */
			obj->timeout--;

			/* Hack -- notice interesting fuel steps */
			if ((obj->timeout < 100) || (!(obj->timeout % 100)))
				/* Redraw stuff */
				player->upkeep->redraw |= (PR_EQUIP);

			/* Hack -- Special treatment when blind */
			if (player->timed[TMD_BLIND]) {
				/* Hack -- save some light for later */
				if (obj->timeout == 0) obj->timeout++;
			} else if (obj->timeout == 0) {
				/* The light is now out */
				disturb(player, 0);
				msg("Your light has gone out!");

				/* If it's a torch, now is the time to delete it */
				if (of_has(obj->flags, OF_BURNS_OUT)) {
					gear_excise_object(obj);
					object_delete(obj);
				}
			} else if ((obj->timeout < 50) && (!(obj->timeout % 20))) {
				/* The light is getting dim */
				disturb(player, 0);
				msg("Your light is growing faint.");
			}
		}
	}

	/* Calculate torch radius */
	player->upkeep->update |= (PU_TORCH);
}


/**
 * Return TRUE if the player can cast a spell.
 *
 * \param p is the player
 * \param show_msg should be set to TRUE if a failure message should be
 * displayed.
 */
bool player_can_cast(struct player *p, bool show_msg)
{
	if (p->class->magic.spell_realm->index == REALM_NONE)
	{
		if (show_msg)
			msg("You cannot pray or produce magics.");

		return FALSE;
	}

	if (p->timed[TMD_BLIND] || no_light())
	{
		if (show_msg)
			msg("You cannot see!");

		return FALSE;
	}

	if (p->timed[TMD_CONFUSED])
	{
		if (show_msg)
			msg("You are too confused!");

		return FALSE;
	}

	return TRUE;
}

/**
 * Return TRUE if the player can study a spell.
 *
 * \param p is the player
 * \param show_msg should be set to TRUE if a failure message should be
 * displayed.
 */
bool player_can_study(struct player *p, bool show_msg)
{
	if (!player_can_cast(p, show_msg))
		return FALSE;

	if (!p->upkeep->new_spells)
	{
		if (show_msg) {
			const char *name = p->class->magic.spell_realm->spell_noun;
			msg("You cannot learn any new %ss!", name);
		}

		return FALSE;
	}

	return TRUE;
}

/**
 * Return TRUE if the player can read scrolls or books.
 *
 * \param p is the player
 * \param show_msg should be set to TRUE if a failure message should be
 * displayed.
 */
bool player_can_read(struct player *p, bool show_msg)
{
	if (p->timed[TMD_BLIND]) {
		if (show_msg)
			msg("You can't see anything.");

		return FALSE;
	}

	if (no_light()) {
		if (show_msg)
			msg("You have no light to read by.");

		return FALSE;
	}

	if (p->timed[TMD_CONFUSED]) {
		if (show_msg)
			msg("You are too confused to read!");

		return FALSE;
	}

	if (p->timed[TMD_AMNESIA]) {
		if (show_msg)
			msg("You can't remember how to read!");

		return FALSE;
	}

	return TRUE;
}

/**
 * Return TRUE if the player can fire something with a launcher.
 *
 * \param p is the player
 * \param show_msg should be set to TRUE if a failure message should be
 * displayed.
 */
bool player_can_fire(struct player *p, bool show_msg)
{
	object_type *obj = equipped_item_by_slot_name(player, "shooting");

	/* Require a usable launcher */
	if (!obj || !p->state.ammo_tval)
	{
		if (show_msg)
			msg("You have nothing to fire with.");

		return FALSE;
	}

	return TRUE;
}

/**
 * Return TRUE if the player can refuel their light source.
 *
 * \param p is the player
 * \param show_msg should be set to TRUE if a failure message should be
 * displayed.
 */
bool player_can_refuel(struct player *p, bool show_msg)
{
	object_type *obj = equipped_item_by_slot_name(player, "light");

	if (obj && of_has(obj->flags, OF_TAKES_FUEL))
		return TRUE;

	if (show_msg)
		msg("Your light cannot be refuelled.");

	return FALSE;
}

/**
 * Prerequiste function for command. See struct cmd_info in cmd-process.c.
 */
bool player_can_cast_prereq(void)
{
	return player_can_cast(player, TRUE);
}

/**
 * Prerequiste function for command. See struct cmd_info in cmd-process.c.
 */
bool player_can_study_prereq(void)
{
	return player_can_study(player, TRUE);
}

/**
 * Prerequiste function for command. See struct cmd_info in cmd-process.c.
 */
bool player_can_read_prereq(void)
{
	return player_can_read(player, TRUE);
}

/**
 * Prerequiste function for command. See struct cmd_info in cmd-process.c.
 */
bool player_can_fire_prereq(void)
{
	return player_can_fire(player, TRUE);
}

/**
 * Prerequiste function for command. See struct cmd_info in cmd-process.c.
 */
bool player_can_refuel_prereq(void)
{
	return player_can_refuel(player, TRUE);
}

/**
 * Return TRUE if the player has a book in their inventory that has unlearned
 * spells.
 *
 * \param p is the player
 */
bool player_book_has_unlearned_spells(struct player *p)
{
	int i, j;
	const class_book *book;

	/* Check if the player can learn new spells */
	if (!p->upkeep->new_spells)
		return FALSE;

	/* Check through all available books */
	for (i = 0; i < z_info->pack_size; i++) {
		struct object *obj = player->upkeep->inven[i];
		if (!obj || !obj_can_browse(obj)) continue;
		book = object_to_book(player->upkeep->inven[i]);
		if (!book) continue;

		/* Extract spells */
		for (j = 0; j < book->num_spells; j++)
			if (spell_okay_to_study(book->spells[j].sidx))
				/* There is a spell the player can study */
				return TRUE;
	}

	return FALSE;
}

/**
 * Apply confusion, if needed, to a direction
 *
 * Display a message and return TRUE if direction changes.
 */
bool player_confuse_dir(struct player *p, int *dp, bool too)
{
	int dir = *dp;

	if (p->timed[TMD_CONFUSED])
		if ((dir == 5) || (randint0(100) < 75))
			/* Random direction */
			dir = ddd[randint0(8)];

	if (*dp != dir) {
		if (too)
			msg("You are too confused.");
		else
			msg("You are confused.");

		*dp = dir;
		return TRUE;
	}

	return FALSE;
}

/* Resting counter */
int resting;

/**
 * Return TRUE if the provided count is one of the conditional REST_ flags.
 */
bool player_resting_is_special(s16b count)
{
	switch (count) {
		case REST_COMPLETE:
		case REST_ALL_POINTS:
		case REST_SOME_POINTS:
			return TRUE;
	}

	return FALSE;
}

/**
 * Return TRUE if the player is resting.
 */
bool player_is_resting(struct player *p)
{
	return resting > 0 || player_resting_is_special(resting);
}

/**
 * Return the remaining number of resting turns.
 */
s16b player_resting_count(struct player *p)
{
	return resting;
}

/**
 * In order to prevent the regeneration bonus from the first few turns, we have
 * to store the original number of turns the user entered. Otherwise, the first
 * few turns will have the bonus and the last few will not.
 */
static int player_turns_rested = 0;
static bool player_rest_disturb = FALSE;

/**
 * Set the number of resting turns.
 *
 * \param count is the number of turns to rest or one of the REST_ constants.
 */
void player_resting_set_count(struct player *p, s16b count)
{
	/* Cancel if player is disturbed */
	if (player_rest_disturb) {
		resting = 0;
		player_rest_disturb = FALSE;
		return;
	}

	/* Ignore if the rest count is negative. */
	if ((count < 0) && !player_resting_is_special(count)) {
		resting = 0;
		return;
	}

	/* Save the rest code */
	resting = count;

	/* Truncate overlarge values */
	if (resting > 9999) resting = 9999;
}

/**
 * Cancel current rest.
 */
void player_resting_cancel(struct player *p)
{
	player_resting_set_count(p, 0);
	player_turns_rested = 0;
	player_rest_disturb = TRUE;
}

/**
 * Return TRUE if the player should get a regeneration bonus for the current
 * rest.
 */
bool player_resting_can_regenerate(struct player *p)
{
	return player_turns_rested >= REST_REQUIRED_FOR_REGEN ||
		player_resting_is_special(resting);
}

/**
 * Perform one turn of resting. This only handles the bookkeeping of resting
 * itself, and does not calculate any possible other effects of resting (see
 * process_world() for regeneration).
 */
void player_resting_step_turn(struct player *p)
{
	/* Timed rest */
	if (resting > 0) {
		/* Reduce rest count */
		resting--;

		/* Redraw the state */
		p->upkeep->redraw |= (PR_STATE);
	}

	/* Take a turn */
	p->upkeep->energy_use = z_info->move_energy;

	/* Increment the resting counters */
	p->resting_turn++;
	player_turns_rested++;
}

/**
 * Handle the conditions for conditional resting (resting with the REST_
 * constants).
 */
void player_resting_complete_special(struct player *p)
{
	/* Complete resting */
	if (player_resting_is_special(resting)) {
		if (resting == REST_ALL_POINTS) {
			if ((p->chp == p->mhp) && (p->csp == p->msp))
				/* Stop resting */
				disturb(p, 0);
		} else if (resting == REST_COMPLETE) {
			if ((p->chp == p->mhp) && (p->csp == p->msp) &&
				!p->timed[TMD_BLIND] && !p->timed[TMD_CONFUSED] &&
				!p->timed[TMD_POISONED] && !p->timed[TMD_AFRAID] &&
				!p->timed[TMD_TERROR] && !p->timed[TMD_STUN] &&
				!p->timed[TMD_CUT] && !p->timed[TMD_SLOW] &&
				!p->timed[TMD_PARALYZED] && !p->timed[TMD_IMAGE] &&
				!p->word_recall)
				/* Stop resting */
				disturb(p, 0);
		} else if (resting == REST_SOME_POINTS) {
			if ((p->chp == p->mhp) || (p->csp == p->msp))
				/* Stop resting */
				disturb(p, 0);
		}
	}
}

/**
 * Check if the player state has the given OF_ flag.
 */
bool player_of_has(struct player *p, int flag)
{
	assert(p);
	return of_has(p->state.flags, flag);
}

/**
 * Check if the player resists (or better) an element
 */
bool player_resists(struct player *p, int element)
{
	return (p->state.el_info[element].res_level > 0);
}

/**
 * Check if the player resists (or better) an element
 */
bool player_is_immune(struct player *p, int element)
{
	return (p->state.el_info[element].res_level == 3);
}

/*
 * Extract a "direction" which will move one step from the player location
 * towards the given "target" location (or "5" if no motion necessary).
 */
int coords_to_dir(int y, int x)
{
	return (motion_dir(player->py, player->px, y, x));
}

/**
 * Places the player at the given coordinates in the cave.
 */
void player_place(struct chunk *c, struct player *p, int y, int x)
{
	assert(!c->squares[y][x].mon);

	/* Save player location */
	p->py = y;
	p->px = x;

	/* Mark cave grid */
	c->squares[y][x].mon = -1;

	/* Clear stair creation */
	p->upkeep->create_down_stair = FALSE;
	p->upkeep->create_up_stair = FALSE;
}



/*
 * Something has happened to disturb the player.
 *
 * The first arg indicates a major disturbance, which affects search.
 *
 * The second arg is currently unused, but could induce output flush.
 *
 * All disturbance cancels repeated commands, resting, and running.
 * 
 * XXX-AS: Make callers either pass in a command
 * or call cmd_cancel_repeat inside the function calling this
 */
void disturb(struct player *p, int stop_search)
{
	/* Cancel repeated commands */
	cmd_cancel_repeat();

	/* Cancel Resting */
	if (player_is_resting(p)) {
		player_resting_cancel(p);
		p->upkeep->redraw |= PR_STATE;
	}

	/* Cancel running */
	if (p->upkeep->running) {
		p->upkeep->running = 0;

		/* Check for new panel if appropriate */
		if (OPT(center_player))
			event_signal(EVENT_PLAYERMOVED);
		p->upkeep->update |= PU_TORCH;
	}

	/* Cancel searching if requested */
	if (stop_search && p->searching) {
		p->searching = FALSE;
		p->upkeep->update |= PU_BONUS;
		p->upkeep->redraw |= PR_STATE;
	}

	/* Flush input */
	event_signal(EVENT_INPUT_FLUSH);
}
